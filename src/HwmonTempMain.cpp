/*
// Copyright (c) 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <HwmonTempSensor.hpp>
#include <Utils.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

static constexpr float pollRateDefault = 0.5;

static constexpr double maxValuePressure = 120000; // Pascals
static constexpr double minValuePressure = 30000;  // Pascals

static constexpr double maxValueRelativeHumidity = 100; // PercentRH
static constexpr double minValueRelativeHumidity = 0;   // PercentRH

static constexpr double maxValueTemperature = 127;  // DegreesC
static constexpr double minValueTemperature = -128; // DegreesC

namespace fs = std::filesystem;
static auto sensorTypes{
    std::to_array<const char*>({"xyz.openbmc_project.Configuration.DPS310",
                                "xyz.openbmc_project.Configuration.EMC1412",
                                "xyz.openbmc_project.Configuration.EMC1413",
                                "xyz.openbmc_project.Configuration.EMC1414",
                                "xyz.openbmc_project.Configuration.HDC1080",
                                "xyz.openbmc_project.Configuration.JC42",
                                "xyz.openbmc_project.Configuration.LM75A",
                                "xyz.openbmc_project.Configuration.LM95234",
                                "xyz.openbmc_project.Configuration.MAX31725",
                                "xyz.openbmc_project.Configuration.MAX31730",
                                "xyz.openbmc_project.Configuration.MAX6581",
                                "xyz.openbmc_project.Configuration.MAX6654",
                                "xyz.openbmc_project.Configuration.NCT7802",
                                "xyz.openbmc_project.Configuration.SBTSI",
                                "xyz.openbmc_project.Configuration.SI7020",
                                "xyz.openbmc_project.Configuration.TMP112",
                                "xyz.openbmc_project.Configuration.TMP175",
                                "xyz.openbmc_project.Configuration.TMP421",
                                "xyz.openbmc_project.Configuration.TMP441",
                                "xyz.openbmc_project.Configuration.TMP75",
                                "xyz.openbmc_project.Configuration.W83773G"})};

static struct SensorParams
    getSensorParameters(const std::filesystem::path& path)
{
    // offset is to default to 0 and scale to 1, see lore
    // https://lore.kernel.org/linux-iio/5c79425f-6e88-36b6-cdfe-4080738d039f@metafoo.de/
    struct SensorParams tmpSensorParameters = {.minValue = minValueTemperature,
                                               .maxValue = maxValueTemperature,
                                               .offsetValue = 0.0,
                                               .scaleValue = 1.0,
                                               .units =
                                                   sensor_paths::unitDegreesC,
                                               .typeName = "temperature"};

    // For IIO RAW sensors we get a raw_value, an offset, and scale
    // to compute the value = (raw_value + offset) * scale
    // with a _raw IIO device we need to get the
    // offsetValue and scaleValue from the driver
    // these are used to compute the reading in
    // units that have yet to be scaled for D-Bus.
    const std::string pathStr = path.string();
    if (pathStr.ends_with("_raw"))
    {
        std::string pathOffsetStr =
            pathStr.substr(0, pathStr.size() - 4) + "_offset";
        std::optional<double> tmpOffsetValue = readFile(pathOffsetStr, 1.0);
        // In case there is nothing to read skip this device
        // This is not an error condition see lore
        // https://lore.kernel.org/linux-iio/5c79425f-6e88-36b6-cdfe-4080738d039f@metafoo.de/
        if (tmpOffsetValue)
        {
            tmpSensorParameters.offsetValue = *tmpOffsetValue;
        }

        std::string pathScaleStr =
            pathStr.substr(0, pathStr.size() - 4) + "_scale";
        std::optional<double> tmpScaleValue = readFile(pathScaleStr, 1.0);
        // In case there is nothing to read skip this device
        // This is not an error condition see lore
        // https://lore.kernel.org/linux-iio/5c79425f-6e88-36b6-cdfe-4080738d039f@metafoo.de/
        if (tmpScaleValue)
        {
            tmpSensorParameters.scaleValue = *tmpScaleValue;
        }
    }

    // Temperatures are read in milli degrees Celsius, we need
    // degrees Celsius. Pressures are read in kilopascal, we need
    // Pascals.  On D-Bus for Open BMC we use the International
    // System of Units without prefixes. Links to the kernel
    // documentation:
    // https://www.kernel.org/doc/Documentation/hwmon/sysfs-interface
    // https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-bus-iio
    if (path.filename() == "in_pressure_input" ||
        path.filename() == "in_pressure_raw")
    {
        tmpSensorParameters.minValue = minValuePressure;
        tmpSensorParameters.maxValue = maxValuePressure;
        // Pressures are read in kilopascal, we need Pascals.
        tmpSensorParameters.scaleValue *= 1000.0;
        tmpSensorParameters.typeName = "pressure";
        tmpSensorParameters.units = sensor_paths::unitPascals;
    }
    else if (path.filename() == "in_humidityrelative_input" ||
             path.filename() == "in_humidityrelative_raw")
    {
        tmpSensorParameters.minValue = minValueRelativeHumidity;
        tmpSensorParameters.maxValue = maxValueRelativeHumidity;
        // Relative Humidity are read in milli-percent, we need percent.
        tmpSensorParameters.scaleValue *= 0.001;
        tmpSensorParameters.typeName = "humidity";
        tmpSensorParameters.units = "PercentRH";
    }
    else
    {
        // Temperatures are read in milli degrees Celsius,
        // we need degrees Celsius.
        tmpSensorParameters.scaleValue *= 0.001;
    }

    return tmpSensorParameters;
}

void createSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::shared_ptr<HwmonTempSensor>>&
        sensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const std::shared_ptr<boost::container::flat_set<std::string>>&
        sensorsChanged)
{
    auto getter = std::make_shared<GetSensorConfiguration>(
        dbusConnection,
        [&io, &objectServer, &sensors, &dbusConnection,
         sensorsChanged](const ManagedObjectType& sensorConfigurations) {
            bool firstScan = sensorsChanged == nullptr;

            // IIO _raw devices look like this on sysfs:
            //     /sys/bus/iio/devices/iio:device0/in_temp_raw
            //     /sys/bus/iio/devices/iio:device0/in_temp_offset
            //     /sys/bus/iio/devices/iio:device0/in_temp_scale
            //
            // Other IIO devices look like this on sysfs:
            //     /sys/bus/iio/devices/iio:device1/in_temp_input
            //     /sys/bus/iio/devices/iio:device1/in_pressure_input
            std::vector<fs::path> paths;
            fs::path root("/sys/bus/iio/devices");
            findFiles(root, R"(in_temp\d*_(input|raw))", paths);
            findFiles(root, R"(in_pressure\d*_(input|raw))", paths);
            findFiles(root, R"(in_humidityrelative\d*_(input|raw))", paths);
            findFiles(fs::path("/sys/class/hwmon"), R"(temp\d+_input)", paths);

            if (paths.empty())
            {
                return;
            }

            // iterate through all found temp and pressure sensors,
            // and try to match them with configuration
            for (auto& path : paths)
            {
                std::smatch match;
                const std::string pathStr = path.string();
                auto directory = path.parent_path();
                fs::path device;

                std::string deviceName;
                if (pathStr.starts_with("/sys/bus/iio/devices"))
                {
                    device = fs::canonical(directory);
                    deviceName = device.parent_path().stem();
                }
                else
                {
                    device = directory / "device";
                    deviceName = fs::canonical(device).stem();
                }
                auto findHyphen = deviceName.find('-');
                if (findHyphen == std::string::npos)
                {
                    std::cerr << "found bad device " << deviceName << "\n";
                    continue;
                }
                std::string busStr = deviceName.substr(0, findHyphen);
                std::string addrStr = deviceName.substr(findHyphen + 1);

                size_t bus = 0;
                size_t addr = 0;
                try
                {
                    bus = std::stoi(busStr);
                    addr = std::stoi(addrStr, nullptr, 16);
                }
                catch (const std::invalid_argument&)
                {
                    continue;
                }
                const SensorData* sensorData = nullptr;
                const std::string* interfacePath = nullptr;
                const char* sensorType = nullptr;
                const SensorBaseConfiguration* baseConfiguration = nullptr;
                const SensorBaseConfigMap* baseConfigMap = nullptr;

                auto thisSensorParameters = getSensorParameters(path);

                for (const std::pair<sdbusplus::message::object_path,
                                     SensorData>& sensor : sensorConfigurations)
                {
                    sensorData = &(sensor.second);
                    for (const char* type : sensorTypes)
                    {
                        auto sensorBase = sensorData->find(type);
                        if (sensorBase != sensorData->end())
                        {
                            baseConfiguration = &(*sensorBase);
                            sensorType = type;
                            break;
                        }
                    }
                    if (baseConfiguration == nullptr)
                    {
                        std::cerr << "error finding base configuration for "
                                  << deviceName << "\n";
                        continue;
                    }
                    baseConfigMap = &baseConfiguration->second;
                    auto configurationBus = baseConfigMap->find("Bus");
                    auto configurationAddress = baseConfigMap->find("Address");

                    if (configurationBus == baseConfigMap->end() ||
                        configurationAddress == baseConfigMap->end())
                    {
                        std::cerr << "error finding bus or address in "
                                     "configuration\n";
                        continue;
                    }

                    if (std::get<uint64_t>(configurationBus->second) != bus ||
                        std::get<uint64_t>(configurationAddress->second) !=
                            addr)
                    {
                        continue;
                    }

                    interfacePath = &(sensor.first.str);
                    break;
                }
                if (interfacePath == nullptr)
                {
                    continue;
                }

                // Temperature has "Name", pressure has "Name1"
                auto findSensorName = baseConfigMap->find("Name");
                if (thisSensorParameters.typeName == "pressure" ||
                    thisSensorParameters.typeName == "humidity")
                {
                    findSensorName = baseConfigMap->find("Name1");
                }

                if (findSensorName == baseConfigMap->end())
                {
                    std::cerr << "could not determine configuration name for "
                              << deviceName << "\n";
                    continue;
                }
                std::string sensorName =
                    std::get<std::string>(findSensorName->second);
                // on rescans, only update sensors we were signaled by
                auto findSensor = sensors.find(sensorName);
                if (!firstScan && findSensor != sensors.end())
                {
                    bool found = false;
                    auto it = sensorsChanged->begin();
                    while (it != sensorsChanged->end())
                    {
                        if (boost::ends_with(*it, findSensor->second->name))
                        {
                            it = sensorsChanged->erase(it);
                            findSensor->second = nullptr;
                            found = true;
                            break;
                        }
                        ++it;
                    }
                    if (!found)
                    {
                        continue;
                    }
                }

                std::vector<thresholds::Threshold> sensorThresholds;
                int index = 1;

                if (!parseThresholdsFromConfig(*sensorData, sensorThresholds,
                                               nullptr, &index))
                {
                    std::cerr << "error populating thresholds for "
                              << sensorName << " index 1\n";
                }

                auto findPollRate = baseConfiguration->second.find("PollRate");
                float pollRate = pollRateDefault;
                if (findPollRate != baseConfiguration->second.end())
                {
                    pollRate = std::visit(VariantToFloatVisitor(),
                                          findPollRate->second);
                    if (pollRate <= 0.0f)
                    {
                        pollRate = pollRateDefault; // polling time too short
                    }
                }

                auto findPowerOn = baseConfiguration->second.find("PowerState");
                PowerState readState = PowerState::always;
                if (findPowerOn != baseConfiguration->second.end())
                {
                    std::string powerState = std::visit(
                        VariantToStringVisitor(), findPowerOn->second);
                    setReadState(powerState, readState);
                }

                auto permitSet = getPermitSet(*baseConfigMap);
                auto& sensor = sensors[sensorName];
                sensor = nullptr;
                auto hwmonFile = getFullHwmonFilePath(directory.string(),
                                                      "temp1", permitSet);
                if (pathStr.starts_with("/sys/bus/iio/devices"))
                {
                    hwmonFile = pathStr;
                }
                if (hwmonFile)
                {
                    sensor = std::make_shared<HwmonTempSensor>(
                        *hwmonFile, sensorType, objectServer, dbusConnection,
                        io, sensorName, std::move(sensorThresholds),
                        thisSensorParameters, pollRate, *interfacePath,
                        readState);
                    sensor->setupRead();
                }
                // Looking for keys like "Name1" for temp2_input,
                // "Name2" for temp3_input, etc.
                int i = 0;
                while (true)
                {
                    ++i;
                    auto findKey =
                        baseConfigMap->find("Name" + std::to_string(i));
                    if (findKey == baseConfigMap->end())
                    {
                        break;
                    }
                    std::string sensorName =
                        std::get<std::string>(findKey->second);
                    hwmonFile = getFullHwmonFilePath(
                        directory.string(), "temp" + std::to_string(i + 1),
                        permitSet);
                    if (pathStr.starts_with("/sys/bus/iio/devices"))
                    {
                        continue;
                    }
                    if (hwmonFile)
                    {
                        // To look up thresholds for these additional sensors,
                        // match on the Index property in the threshold data
                        // where the index comes from the sysfs file we're on,
                        // i.e. index = 2 for temp2_input.
                        int index = i + 1;
                        std::vector<thresholds::Threshold> thresholds;

                        if (!parseThresholdsFromConfig(*sensorData, thresholds,
                                                       nullptr, &index))
                        {
                            std::cerr << "error populating thresholds for "
                                      << sensorName << " index " << index
                                      << "\n";
                        }

                        auto& sensor = sensors[sensorName];
                        sensor = nullptr;
                        sensor = std::make_shared<HwmonTempSensor>(
                            *hwmonFile, sensorType, objectServer,
                            dbusConnection, io, sensorName,
                            std::move(thresholds), thisSensorParameters,
                            pollRate, *interfacePath, readState);
                        sensor->setupRead();
                    }
                }
            }
        });
    getter->getConfiguration(
        std::vector<std::string>(sensorTypes.begin(), sensorTypes.end()));
}

void interfaceRemoved(
    sdbusplus::message::message& message,
    boost::container::flat_map<std::string, std::shared_ptr<HwmonTempSensor>>&
        sensors)
{
    if (message.is_method_error())
    {
        std::cerr << "interfacesRemoved callback method error\n";
        return;
    }

    sdbusplus::message::object_path path;
    std::vector<std::string> interfaces;

    message.read(path, interfaces);

    // If the xyz.openbmc_project.Confguration.X interface was removed
    // for one or more sensors, delete those sensor objects.
    auto sensorIt = sensors.begin();
    while (sensorIt != sensors.end())
    {
        if ((sensorIt->second->configurationPath == path) &&
            (std::find(interfaces.begin(), interfaces.end(),
                       sensorIt->second->objectType) != interfaces.end()))
        {
            sensorIt = sensors.erase(sensorIt);
        }
        else
        {
            sensorIt++;
        }
    }
}

int main()
{
    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name("xyz.openbmc_project.HwmonTempSensor");
    sdbusplus::asio::object_server objectServer(systemBus);
    boost::container::flat_map<std::string, std::shared_ptr<HwmonTempSensor>>
        sensors;
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;
    auto sensorsChanged =
        std::make_shared<boost::container::flat_set<std::string>>();

    io.post([&]() {
        createSensors(io, objectServer, sensors, systemBus, nullptr);
    });

    boost::asio::deadline_timer filterTimer(io);
    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message& message) {
            if (message.is_method_error())
            {
                std::cerr << "callback method error\n";
                return;
            }
            sensorsChanged->insert(message.get_path());
            // this implicitly cancels the timer
            filterTimer.expires_from_now(boost::posix_time::seconds(1));

            filterTimer.async_wait([&](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    /* we were canceled*/
                    return;
                }
                if (ec)
                {
                    std::cerr << "timer error\n";
                    return;
                }
                createSensors(io, objectServer, sensors, systemBus,
                              sensorsChanged);
            });
        };

    for (const char* type : sensorTypes)
    {
        auto match = std::make_unique<sdbusplus::bus::match::match>(
            static_cast<sdbusplus::bus::bus&>(*systemBus),
            "type='signal',member='PropertiesChanged',path_namespace='" +
                std::string(inventoryPath) + "',arg0namespace='" + type + "'",
            eventHandler);
        matches.emplace_back(std::move(match));
    }

    setupManufacturingModeMatch(*systemBus);

    // Watch for entity-manager to remove configuration interfaces
    // so the corresponding sensors can be removed.
    auto ifaceRemovedMatch = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*systemBus),
        "type='signal',member='InterfacesRemoved',arg0path='" +
            std::string(inventoryPath) + "/'",
        [&sensors](sdbusplus::message::message& msg) {
            interfaceRemoved(msg, sensors);
        });

    matches.emplace_back(std::move(ifaceRemovedMatch));

    io.run();
}
