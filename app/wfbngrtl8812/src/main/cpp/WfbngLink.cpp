#include "WfbngLink.hpp"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <jni.h>

#include "RxFrame.h"
#include "SignalQualityCalculator.h"
#include "TxFrame.h"
#include "libusb.h"
#include "wfb-ng/src/wifibroadcast.hpp"

#include <cstdint>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <list>
#include <mutex>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <thread>

std::string generate_random_string(size_t length) {
    const std::string characters = "abcdefghijklmnopqrstuvwxyz";
    std::random_device rd;  // Obtain a random number from hardware
    std::mt19937 gen(rd()); // Seed the generator
    std::uniform_int_distribution<> distrib(0, characters.size() - 1);

    std::string result;
    result.reserve(length); // Reserve space to avoid multiple allocations

    for (size_t i = 0; i < length; ++i) {
        result += characters[distrib(gen)];
    }

    return result;
}

#undef TAG
#define TAG "pixelpilot"

std::string uint8_to_hex_string(const uint8_t *v, const size_t s) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < s; i++) {
        ss << std::hex << std::setw(2) << static_cast<int>(v[i]);
    }
    return ss.str();
}

WfbngLink::WfbngLink(JNIEnv *env, jobject context) {
    initAgg();
    Logger_t log;
    wifi_driver = std::make_unique<WiFiDriver>(log);
}
void WfbngLink::initAgg() {
    std::string client_addr = "127.0.0.1";
    uint32_t link_id = 7669206; // sha1 hash of link_domain="default"
    uint64_t epoch = 0;

    int video_client_port = 5600;
    uint8_t video_radio_port = 0;
    uint32_t video_channel_id_f = (link_id << 8) + video_radio_port;
    video_channel_id_be = htobe32(video_channel_id_f);
    video_aggregator = std::make_unique<Aggregator>(client_addr, video_client_port, keyPath, epoch, video_channel_id_f);

    int mavlink_client_port = 14550;
    uint8_t mavlink_radio_port = 0x10;
    uint32_t mavlink_channel_id_f = (link_id << 8) + mavlink_radio_port;
    mavlink_channel_id_be = htobe32(mavlink_channel_id_f);
    mavlink_aggregator =
        std::make_unique<Aggregator>(client_addr, mavlink_client_port, keyPath, epoch, mavlink_channel_id_f);

    int udp_client_port = 8000;
    uint8_t udp_radio_port = wfb_rx_port;
    uint32_t udp_channel_id_f = (link_id << 8) + udp_radio_port;
    udp_channel_id_be = htobe32(udp_channel_id_f);

    udp_aggregator = std::make_unique<Aggregator>(client_addr, udp_client_port, keyPath, epoch, udp_channel_id_f);
}

int WfbngLink::run(JNIEnv *env, jobject context, jint wifiChannel, jint bw, jint fd) {
    int r;
    libusb_context *ctx = NULL;
    std::unique_ptr<TxFrame> txFrame = std::make_unique<TxFrame>();

    r = libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);

    r = libusb_init(&ctx);
    if (r < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to init libusb.");
        return r;
    }

    // Open adapters
    struct libusb_device_handle *dev_handle;
    r = libusb_wrap_sys_device(ctx, (intptr_t)fd, &dev_handle);
    if (r < 0) {
        libusb_exit(ctx);
        return r;
    }

    /*Check if kernel driver attached*/
    if (libusb_kernel_driver_active(dev_handle, 0)) {
        r = libusb_detach_kernel_driver(dev_handle, 0); // detach driver
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "libusb_detach_kernel_driver: %d", r);
    }
    r = libusb_claim_interface(dev_handle, 0);
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Creating driver and device for fd=%d", fd);

    rtl_devices.emplace(fd, wifi_driver->CreateRtlDevice(dev_handle));
    if (!rtl_devices.at(fd)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "CreateRtlDevice error");
        return -1;
    }

    uint8_t *video_channel_id_be8 = reinterpret_cast<uint8_t *>(&video_channel_id_be);
    uint8_t *udp_channel_id_be8 = reinterpret_cast<uint8_t *>(&udp_channel_id_be);
    uint8_t *mavlink_channel_id_be8 = reinterpret_cast<uint8_t *>(&mavlink_channel_id_be);

    try {
        auto packetProcessor = [this, video_channel_id_be8, mavlink_channel_id_be8, udp_channel_id_be8](
                                   const Packet &packet) {
            RxFrame frame(packet.Data);
            if (!frame.IsValidWfbFrame()) {
                return;
            }

            int8_t rssi[4] = {(int8_t)packet.RxAtrib.rssi[0], (int8_t)packet.RxAtrib.rssi[1], 1, 1};

            // __android_log_print(ANDROID_LOG_WARN, TAG, "rssi1 %d, rssi2 %d", (int)rssi[0], (int)rssi[1]);

            uint32_t freq = 0;
            int8_t noise[4] = {1, 1, 1, 1};
            uint8_t antenna[4] = {1, 1, 1, 1};

            std::lock_guard<std::mutex> lock(agg_mutex);
            if (frame.MatchesChannelID(video_channel_id_be8)) {
                SignalQualityCalculator::get_instance().add_rssi(packet.RxAtrib.rssi[0], packet.RxAtrib.rssi[1]);
                video_aggregator->process_packet(packet.Data.data() + sizeof(ieee80211_header),
                                                 packet.Data.size() - sizeof(ieee80211_header) - 4,
                                                 0,
                                                 antenna,
                                                 rssi,
                                                 noise,
                                                 freq,
                                                 0,
                                                 0,
                                                 NULL);
            } else if (frame.MatchesChannelID(mavlink_channel_id_be8)) {
                mavlink_aggregator->process_packet(packet.Data.data() + sizeof(ieee80211_header),
                                                   packet.Data.size() - sizeof(ieee80211_header) - 4,
                                                   0,
                                                   antenna,
                                                   rssi,
                                                   noise,
                                                   freq,
                                                   0,
                                                   0,
                                                   NULL);
            } else if (frame.MatchesChannelID(udp_channel_id_be8)) {
                __android_log_print(ANDROID_LOG_WARN, TAG, "Received UDP packet");

                udp_aggregator->process_packet(packet.Data.data() + sizeof(ieee80211_header),
                                               packet.Data.size() - sizeof(ieee80211_header) - 4,
                                               0,
                                               antenna,
                                               rssi,
                                               noise,
                                               freq,
                                               0,
                                               0,
                                               NULL);
            } else {
                std::stringstream ss;
                frame.printChannelId(ss);
                __android_log_print(ANDROID_LOG_WARN, TAG, "Received unknown packet, channel ID: %s", ss.str().c_str());
            }
        };

        if (!usb_event_thread) {
            usb_event_thread = std::make_unique<std::thread>([ctx, this, &fd] {
                while (true) {
                    auto dev = this->rtl_devices.at(fd).get();
                    if (dev == nullptr) break;
                    if (dev->should_stop) break;
                    int r = libusb_handle_events(ctx);
                    if (r < 0) {
                        this->log->error("Error handling events: {}", r);
                        break;
                    }
                }
            });

            std::shared_ptr<TxArgs> args = std::make_shared<TxArgs>();
            args->udp_port = 8001;
            args->link_id = link_id;
            args->keypair = keyPath;
            args->stbc = true;
            args->ldpc = true;
            args->mcs_index = 2;
            args->vht_mode = false;
            args->short_gi = false;
            args->bandwidth = 20; // TODOII: need to adjust according to bw??
            args->k = 1;
            args->n = 2;

            args->radio_port = wfb_tx_port;

            __android_log_print(
                ANDROID_LOG_ERROR, TAG, "radio link ID %d,radio PORT %d", args->link_id, args->radio_port);

            Rtl8812aDevice *current_device = rtl_devices.at(fd).get();
            TxFrame *tx_frame = txFrame.get();
            if (!usb_tx_thread) {
                usb_tx_thread = std::make_unique<std::thread>([tx_frame, current_device, args] {
                    tx_frame->run(current_device, args.get());
                    __android_log_print(ANDROID_LOG_DEBUG, TAG, "usb_transfer thread should terminate");
                });
            }

            start_link_quality_thread(fd);
        }

        auto bandWidth = (bw == 20 ? CHANNEL_WIDTH_20 : CHANNEL_WIDTH_40);
        rtl_devices.at(fd)->Init(packetProcessor,
                                 SelectedChannel{
                                     .Channel = static_cast<uint8_t>(wifiChannel),
                                     .ChannelOffset = 0,
                                     .ChannelWidth = bandWidth,
                                 });
    } catch (const std::runtime_error &error) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "runtime_error: %s", error.what());

        auto dev = rtl_devices.at(fd).get();
        if (dev) {
            dev->should_stop = true;
        }
        txFrame->stop();
        usb_tx_thread->join();
        usb_event_thread->join();

        // Join the thread (optional if you want to wait for the thread to finish)
        link_quality_thread->join();

        return -1;
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Init done, releasing...");

    auto dev = rtl_devices.at(fd).get();
    if (dev) {
        dev->should_stop = true;
    }
    txFrame->stop();
    usb_tx_thread->join();
    usb_event_thread->join();

    r = libusb_release_interface(dev_handle, 0);
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "libusb_release_interface: %d", r);

    libusb_exit(ctx);
    return 0;
}

void WfbngLink::stop(JNIEnv *env, jobject context, jint fd) {
    auto dev = rtl_devices.at(fd).get();
    if (dev) {
        dev->should_stop = true;
    }
}

//----------------------------------------------------JAVA
// bindings---------------------------------------------------------------
inline jlong jptr(WfbngLink *wfbngLinkN) { return reinterpret_cast<intptr_t>(wfbngLinkN); }

inline WfbngLink *native(jlong ptr) { return reinterpret_cast<WfbngLink *>(ptr); }

inline std::list<int> toList(JNIEnv *env, jobject list) {
    // Get the class and method IDs for java.util.List and its methods
    jclass listClass = env->GetObjectClass(list);
    jmethodID sizeMethod = env->GetMethodID(listClass, "size", "()I");
    jmethodID getMethod = env->GetMethodID(listClass, "get", "(I)Ljava/lang/Object;");
    // Method ID to get int value from Integer object
    jclass integerClass = env->FindClass("java/lang/Integer");
    jmethodID intValueMethod = env->GetMethodID(integerClass, "intValue", "()I");

    // Get the size of the list
    jint size = env->CallIntMethod(list, sizeMethod);

    // Create a C++ list to store the elements
    std::list<int> res;

    // Iterate over the list and add elements to the C++ list
    for (int i = 0; i < size; ++i) {
        // Get the element at index i
        jobject element = env->CallObjectMethod(list, getMethod, i);
        // Convert the element to int
        jint value = env->CallIntMethod(element, intValueMethod);
        // Add the element to the C++ list
        res.push_back(value);
    }

    return res;
}

extern "C" JNIEXPORT jlong JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeInitialize(JNIEnv *env,
                                                                                            jclass clazz,
                                                                                            jobject context) {
    // rmt_CreateGlobalInstance(&rmt);
    // rmt_ScopedCPUSample(StartWfb, 0);
    auto *p = new WfbngLink(env, context);
    return jptr(p);
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeRun(
    JNIEnv *env, jclass clazz, jlong wfbngLinkN, jobject androidContext, jint wifiChannel, int bandWidth, jint fd) {
    native(wfbngLinkN)->run(env, androidContext, wifiChannel, bandWidth, fd);
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeStartAdaptivelink(JNIEnv *env,
                                                                                                  jclass clazz,
                                                                                                  jlong wfbngLinkN) {
    if (native(wfbngLinkN)->video_aggregator == nullptr) {
        return;
    }

    auto aggregator = native(wfbngLinkN)->video_aggregator.get();
}

extern "C" JNIEXPORT jint JNICALL Java_com_openipc_pixelpilot_UsbSerialService_nativeGetSignalQuality(JNIEnv *env,
                                                                                                      jclass clazz) {
    return SignalQualityCalculator::get_instance().calculate_signal_quality().quality;
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeStop(
    JNIEnv *env, jclass clazz, jlong wfbngLinkN, jobject androidContext, jint fd) {
    native(wfbngLinkN)->stop(env, androidContext, fd);
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeCallBack(JNIEnv *env,
                                                                                         jclass clazz,
                                                                                         jobject wfbStatChangedI,
                                                                                         jlong wfbngLinkN) {
    if (native(wfbngLinkN)->video_aggregator == nullptr) {
        return;
    }
    auto aggregator = native(wfbngLinkN)->video_aggregator.get();

    jclass jClassExtendsIWfbStatChangedI = env->GetObjectClass(wfbStatChangedI);
    jclass jcStats = env->FindClass("com/openipc/wfbngrtl8812/WfbNGStats");
    if (jcStats == nullptr) {
        return;
    }
    jmethodID jcStatsConstructor = env->GetMethodID(jcStats, "<init>", "(IIIIIIII)V");
    if (jcStatsConstructor == nullptr) {
        return;
    }

    SignalQualityCalculator::get_instance().add_fec_data(
        aggregator->count_p_all, aggregator->count_p_fec_recovered, aggregator->count_p_lost);

    auto stats = env->NewObject(jcStats,
                                jcStatsConstructor,
                                (jint)aggregator->count_p_all,
                                (jint)aggregator->count_p_dec_err,
                                (jint)aggregator->count_p_dec_ok,
                                (jint)aggregator->count_p_fec_recovered,
                                (jint)aggregator->count_p_lost,
                                (jint)aggregator->count_p_bad,
                                (jint)aggregator->count_p_override,
                                (jint)aggregator->count_p_outgoing);
    if (stats == nullptr) {
        return;
    }
    jmethodID onStatsChanged = env->GetMethodID(
        jClassExtendsIWfbStatChangedI, "onWfbNgStatsChanged", "(Lcom/openipc/wfbngrtl8812/WfbNGStats;)V");
    if (onStatsChanged == nullptr) {
        return;
    }
    env->CallVoidMethod(wfbStatChangedI, onStatsChanged, stats);
    aggregator->clear_stats();
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeRefreshKey(JNIEnv *env,
                                                                                           jclass clazz,
                                                                                           jlong wfbngLinkN) {
    native(wfbngLinkN)->initAgg();
}

void WfbngLink::start_link_quality_thread(int fd) {
    link_quality_thread = std::make_unique<std::thread>([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(10));

        const char *ip = "10.5.0.10";
        int port = 9999;

        int sockfd;
        struct sockaddr_in server_addr;

        // Create UDP socket
        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Socket creation failed");
            return;
        }
        int opt = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Invalid IP address");
            close(sockfd);
            return;
        }

        int sockfd2;
        struct sockaddr_in server_addr2;

        // Create UDP socket
        if ((sockfd2 = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Socket creation failed");
            return;
        }
        int opt2 = 1;
        setsockopt(sockfd2, SOL_SOCKET, SO_REUSEADDR, &opt2, sizeof(opt2));

        memset(&server_addr2, 0, sizeof(server_addr2));
        server_addr2.sin_family = AF_INET;
        server_addr2.sin_port = htons(7755);
        if (inet_pton(AF_INET, ip, &server_addr2.sin_addr) <= 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Invalid IP address");
            close(sockfd);
            return;
        }

        // Send message repeatedly every 0.1 seconds
        while (true) {

            auto quality = SignalQualityCalculator::get_instance().calculate_signal_quality();

#if defined(ANDROID_DEBUG_RSSI) || true
            __android_log_print(ANDROID_LOG_WARN, TAG, "quality %d", quality.quality);
#endif

            time_t currentEpoch = time(nullptr);

            const auto map_range =
                [](double value, double inputMin, double inputMax, double outputMin, double outputMax) {
                    return outputMin + ((value - inputMin) * (outputMax - outputMin) / (inputMax - inputMin));
                };

            // map to 1000..2000
            quality.quality = map_range(quality.quality, -1024, 1024, 1000, 2000);
            {
                uint32_t len;

                char message[100];

                snprintf(message + sizeof(len),
                         sizeof(message) - sizeof(len),
                         "%ld:%d:%d:%d:%d:%d:%d:23:20\n",
                         static_cast<long>(currentEpoch),
                         quality.quality,
                         quality.quality,
                         quality.recovered_last_second,
                         quality.lost_last_second,
                         quality.quality,
                         quality.quality);
                len = strlen(message + sizeof(len));
                len = htonl(len);
                memcpy(message, &len, sizeof(len));

                __android_log_print(ANDROID_LOG_ERROR, TAG, "message %s", message + 4);

                ssize_t sent = sendto(sockfd,
                                      message,
                                      strlen(message + sizeof(len)) + sizeof(len),
                                      0,
                                      (struct sockaddr *)&server_addr,
                                      sizeof(server_addr));

                if (sent < 0) {
                    __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to send message");
                    break;
                }
            }

            static int idrCount = 0;
            static char message[100]; // = "10000000:2000:2000:5:10:-70:25:23:20\n";

            if (quality.lost_last_second) {
                idrCount = 10;
            }

            if (idrCount > 0) {
                --idrCount;
                uint32_t len;

                len = strlen("special:request_keyframe:aaaa");
                len = htonl(len);
                memcpy(message, &len, sizeof(len));
                snprintf(message + sizeof(uint32_t),
                         sizeof(message) - sizeof(uint32_t),
                         "special:request_keyframe:%s",
                         generate_random_string(4).c_str());

                ssize_t sent = sendto(sockfd,
                                      message,
                                      strlen(message + sizeof(uint32_t)) + sizeof(uint32_t),
                                      0,
                                      (struct sockaddr *)&server_addr,
                                      sizeof(server_addr));
                if (sent < 0) {
                    __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to send request_keyframe message");
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        close(sockfd);
    });

    rtl_devices.at(fd)->SetTxPower(30);
    //             rtl_devices.at(fd)->SetTxPower(10);
}
