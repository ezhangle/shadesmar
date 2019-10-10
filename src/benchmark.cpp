//
// Created by squadrick on 11/10/19.
//
#include <shadesmar/message.h>
#include <shadesmar/publisher.h>
#include <shadesmar/subscriber.h>
#include <iostream>

#define QUEUE_SIZE 16
#define SECONDS 10
#define VECTOR_SIZE 500000
#define REF true

class BenchmarkMsg : public shm::msg::BaseMsg {
 public:
  std::vector<uint8_t> arr;
  SHM_PACK(arr);
  explicit BenchmarkMsg(int n) {
    for (int i = 0; i < n; ++i) arr.push_back(255);
  }

  BenchmarkMsg() = default;
};

int count = 0;
uint64_t lag = 0;
void callback(const std::shared_ptr<BenchmarkMsg> &msg) {
  ++count;
  lag += std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count() -
         msg->timestamp;
}

int main() {
  if (fork() == 0) {
    sleep(1);
    shm::Subscriber<BenchmarkMsg, QUEUE_SIZE> sub("benchmark", callback, REF);
    auto start = std::chrono::system_clock::now();
    int seconds = 0;
    while (true) {
      sub.spinOnce();
      auto end = std::chrono::system_clock::now();
      auto diff =
          std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
      if (diff.count() > 1000) {
        double lag_ = lag * 10e-9 / count;
        std::cout << "Number of messages sent: " << count << std::endl;
        std::cout << "Average Lag: " << lag_ << " s" << std::endl;

        if (++seconds == SECONDS) break;
        count = 0;
        lag = 0;
        start = std::chrono::system_clock::now();
      }
    }

  } else {
    shm::Publisher<BenchmarkMsg, QUEUE_SIZE> pub("benchmark");
    BenchmarkMsg msg(VECTOR_SIZE);
    msgpack::sbuffer buf;
    msgpack::pack(buf, msg);
    std::cout << "Number of bytes = " << buf.size() << std::endl;
    auto start = std::chrono::system_clock::now();

    while (true) {
      msg.init_time();
      pub.publish(msg);
      auto end = std::chrono::system_clock::now();
      auto diff =
          std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
      if (diff.count() > (SECONDS + 2) * 1000) break;
    }
  }
}