// 适配器类型仅在实现文件内可见；直接编译它以验证真实的 stop/configure 清理路径。
#include "communication/mqtt/mqtt_client_mosquitto.cpp"

#include <iostream>
#include <stdexcept>

namespace edge_controller {
struct MosquittoMqttClientTestAccess {
    static void seed(MosquittoMqttClient& client, std::uint64_t sequence) {
        client.pending_receipts_[1] = sequence;
        client.pending_receipts_[2] = sequence + 1;
        MosquittoMqttClient::on_publish(nullptr, &client, 2);
    }
    static bool empty(const MosquittoMqttClient& client) {
        return client.pending_receipts_.empty() && client.acknowledged_.empty()
            && client.mosq_ == nullptr && !client.lifecycle_;
    }
    static void create_disconnected_client(MosquittoMqttClient& client) {
        client.lifecycle_ = std::make_unique<MosquittoLibraryLifecycle>();
        client.mosq_ = mosquitto_new(nullptr, true, &client);
        if (!client.mosq_) throw std::runtime_error("cannot create MQTT test client");
    }
};
}

int main() {
    using namespace edge_controller;
    try {
        MosquittoMqttClient client;
        for (std::uint64_t cycle = 0; cycle < 100; ++cycle) {
            const auto sequence = (1ULL << 63) | (cycle * 4);
            MosquittoMqttClientTestAccess::seed(client, sequence);
            if (cycle % 2 == 0) MosquittoMqttClientTestAccess::create_disconnected_client(client);
            client.stop();
            if (!MosquittoMqttClientTestAccess::empty(client) || client.consume_publish_ack(sequence + 1))
                throw std::runtime_error("stop retained old MQTT receipts");
            MosquittoMqttClientTestAccess::seed(client, sequence + 2);
            if (cycle % 2 != 0) MosquittoMqttClientTestAccess::create_disconnected_client(client);
            MqttSettings settings;
            settings.enabled = false;
            if (!is_ok(client.configure(settings, nullptr)) || !MosquittoMqttClientTestAccess::empty(client))
                throw std::runtime_error("configure retained old MQTT receipts");
        }
        std::cout << "MQTT lifecycle regression passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
