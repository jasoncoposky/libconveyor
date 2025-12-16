#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>
#include <chrono> // For std::chrono::milliseconds
#include <cassert> // For assert

// Shared resources
std::vector<char> g_buffer;
std::mutex g_mutex;
std::condition_variable g_cv_producer; // Producer waits if buffer is full
std::condition_variable g_cv_consumer; // Consumer waits if buffer is empty
bool g_stop_threads = false;

const size_t BUFFER_CAPACITY = 10; // Small capacity for quick testing

void producer_thread_func() {
    std::cout << "Producer: Thread started." << std::endl << std::flush;
    std::string data_to_produce = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    size_t data_index = 0;

    while (!g_stop_threads && data_index < data_to_produce.length()) {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_cv_producer.wait(lock, [] {
            std::cout << "Producer: Waiting. Buffer size: " << g_buffer.size() << std::endl << std::flush;
            return g_buffer.size() < BUFFER_CAPACITY || g_stop_threads;
        });

        if (g_stop_threads) {
            std::cout << "Producer: Stop signal received, exiting." << std::endl << std::flush;
            break;
        }

        if (g_buffer.size() < BUFFER_CAPACITY) {
            char item = data_to_produce[data_index++];
            g_buffer.push_back(item);
            std::cout << "Producer: Produced '" << item << "'. Buffer size: " << g_buffer.size() << std::endl << std::flush;
            g_cv_consumer.notify_one(); // Notify consumer that data is available
        }
    }
    std::cout << "Producer: Exiting loop. g_stop_threads: " << g_stop_threads << ", data_index: " << data_index << ", data_to_produce.length(): " << data_to_produce.length() << std::endl << std::flush;
    g_cv_consumer.notify_all(); // Ensure consumer doesn't get stuck waiting
}

void consumer_thread_func() {
    std::cout << "Consumer: Thread started." << std::endl << std::flush;
    while (true) {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_cv_consumer.wait(lock, [] {
            std::cout << "Consumer: Waiting. Buffer size: " << g_buffer.size() << std::endl << std::flush;
            return !g_buffer.empty() || g_stop_threads;
        });

        if (g_stop_threads && g_buffer.empty()) {
            std::cout << "Consumer: Stop signal received and buffer empty, exiting." << std::endl << std::flush;
            break;
        }

        if (!g_buffer.empty()) {
            char item = g_buffer.front();
            g_buffer.erase(g_buffer.begin());
            std::cout << "Consumer: Consumed '" << item << "'. Buffer size: " << g_buffer.size() << std::endl << std::flush;
            g_cv_producer.notify_one(); // Notify producer that space is available
        }
    }
    std::cout << "Consumer: Exiting loop. g_stop_threads: " << g_stop_threads << ", buffer empty: " << g_buffer.empty() << std::endl << std::flush;
}

int main() {
    std::cout << "Main: Starting producer-consumer toy model." << std::endl << std::flush;

    std::thread producer(producer_thread_func);
    std::thread consumer(consumer_thread_func);

    // Let threads run for a while
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); 

    // Signal threads to stop
    std::unique_lock<std::mutex> lock(g_mutex); // Lock to set stop flag and notify safely
    g_stop_threads = true;
    lock.unlock(); // Release lock before notifying all to avoid deadlocks
    g_cv_producer.notify_all();
    g_cv_consumer.notify_all();
    
    std::cout << "Main: Signaled threads to stop." << std::endl << std::flush;

    producer.join();
    consumer.join();

    std::cout << "Main: Producer and Consumer threads joined. Toy model finished." << std::endl << std::flush;
    return 0;
}
