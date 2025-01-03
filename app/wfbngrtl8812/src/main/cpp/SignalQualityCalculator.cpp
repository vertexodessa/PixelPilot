#include "SignalQualityCalculator.h"
#include <android/log.h>

float SignalQualityCalculator::get_avg_rssi()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    float avg_rssi1 = 0;
    float avg_rssi2 = 0;
    int count = m_rssis.size();

    if (count > 0)
    {
        for (auto& rssi : m_rssis)
        {
            avg_rssi1 += rssi.first;
            avg_rssi2 += rssi.second;
        }

        avg_rssi1 /= count;
        avg_rssi2 /= count;
    }
    else
    {
        avg_rssi1 = avg_rssi2 = 0;
    }

    m_rssis.resize(0);

    float avg_rssi = std::max(avg_rssi1, avg_rssi2);

    m_rssis.resize(0);

    return avg_rssi;
}

void SignalQualityCalculator::add_rssi(uint8_t ant1, uint8_t ant2)
{
    // __android_log_print(ANDROID_LOG_WARN, TAG, "rssi1 %d, rssi2 %d", (int)ant1,
    // (int)ant2);
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_rssis.push_back({ant1, ant2});
}

float SignalQualityCalculator::calculate_signal_quality()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // signal quality is calculated based on the average RSSI and the number of FEC packets recovered and lost
    // lowest quality is 900, highest is 2000
    // quality is calculated as follows:
    // 1. RSSI is mapped from 30..90 to 1000..2000
    // 2. if p_lost > 0, quality is set to 900 and returned
    // 3. quality is adjusted based on the number of packets recovered and lost:
    //    quality = RSSI - p_recovered * 10

    if (m_fec_data.empty())
    {
        __android_log_print(ANDROID_LOG_DEBUG, "QUALITY", "No FEC data");
        return -1024;
    }

    auto [p_recovered, p_lost] = get_accumulated_fec_data();
//    if (p_lost > 0)
//    {
//        __android_log_print(ANDROID_LOG_DEBUG, "QUALITY", "P_LOST > 0");
//        m_fec_data.clear();
//        return -1024;
//    }
    float avg_rssi = get_avg_rssi();

    avg_rssi = map_range(avg_rssi, 30, 90, -1024, 1024);

    __android_log_print(
        ANDROID_LOG_DEBUG, "QUALITY", "RSSI: %f, P_RECOVERED: %d, P_LOST: %d", avg_rssi, p_recovered, p_lost);

    m_rssis.clear();
    m_fec_data.clear();

    return std::max<int>(-1024, std::min<int>(1024, avg_rssi - p_recovered * 10 - p_lost * 100));
}

std::pair<uint32_t, uint32_t> SignalQualityCalculator::get_accumulated_fec_data()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    uint32_t p_recovered = 0;
    uint32_t p_lost = 0;
    for (const auto& data : m_fec_data)
    {
        p_recovered += data.first;
        p_lost += data.second;
    }
    return {p_recovered, p_lost};
}
void SignalQualityCalculator::add_fec_data(uint32_t p_recovered, uint32_t p_lost)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_fec_data.push_back({p_recovered, p_lost});
}
