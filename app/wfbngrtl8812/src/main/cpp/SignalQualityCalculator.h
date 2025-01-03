#pragma once

#include <cstdint>
#include <mutex>

class SignalQualityCalculator
{
  public:
    void add_rssi(uint8_t ant1, uint8_t ant2);

    void add_fec_data(uint32_t p_recovered, uint32_t p_lost);

    float calculate_signal_quality();

    static SignalQualityCalculator& get_instance()
    {
        static SignalQualityCalculator instance;
        return instance;
    }

  private:
    double map_range(double value, double inputMin, double inputMax, double outputMin, double outputMax)
    {
        return outputMin + ((value - inputMin) * (outputMax - outputMin) / (inputMax - inputMin));
    }

    float get_avg_rssi();

    std::pair<uint32_t, uint32_t> get_accumulated_fec_data();

    std::recursive_mutex m_mutex;
    std::vector<std::pair<uint8_t, uint8_t>> m_rssis;
    std::vector<std::pair<uint32_t, uint32_t>> m_fec_data;
};
