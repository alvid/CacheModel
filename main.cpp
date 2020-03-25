#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <thread>
#include <vector>

class Thread_logger {
public:
    Thread_logger() {
        std::ostringstream oss;
        oss << std::this_thread::get_id() << ".log";

        ofs.open(oss.str(), std::ios_base::ate);
    }
    ~Thread_logger() {
        ofs.close();
    }
    Thread_logger& operator <<(std::string const& str) {
        ofs << str << std::endl;
        ofs.flush();
        return *this;
    }

private:
    std::ofstream ofs;
};

static thread_local Thread_logger tl;

template <typename KeyType, typename UserType>
class Cache_line {
public:
    using Cache_value = std::shared_ptr<UserType>;
    using Callback = std::function<void (UserType *)>;

    Cache_line() {
        memset(&stat, 0, sizeof stat);
    }

    void reset() {
        std::unique_lock wlock(map_mt);
        cache_map.clear();
    }

    Cache_value get_data(KeyType const& key, std::function<void (Callback)> const& asynch_writer)
    {
        {
            // (А) здесь стоят потоки-читатели, ожидающие доступа на чтение к хранилищу
            std::shared_lock rlock(map_mt);
            auto itr = cache_map.find(key);
            if (itr != cache_map.end()) {
                ++stat.hit_count;
                return itr->second;
            }
        }
        ++stat.miss_count;

        // (В) здесь стоят потоки-читатели, ожидающие доступа к очереди активных запросов
        std::unique_lock req_lock(req_mt);
        // защищаемся от двух и более одинаковых запросов данных
        auto r_itr = active_requests.find(key);
        if (r_itr != active_requests.end()) {
            return wait_for_data(key, req_lock);
        }

        // сюда проходит один! поток, который будет вносить данные в хранилище
        active_requests.insert(key);
        req_lock.unlock();

        // вызываем асинхронную функцию создания мастер-объекта вне КС кэша
        asynch_writer(std::bind(&Cache_line<KeyType, UserType>::on_ready_data, this, key, std::placeholders::_1));

        req_lock.lock();
        return wait_for_data(key, req_lock);
    }

    Cache_value wait_for_data(KeyType const& key, std::unique_lock<std::mutex> &req_lock)
    {
        // (C) здесь стоят потоки-читатели, ожидающие выполнения запроса на заполнение хранилища
        req_cv.wait(req_lock, [this, &key] { return active_requests.find(key) == active_requests.end(); });

        // (D) здесь стоят потоки-читатели, получившие сигнал о готовности данных
        std::shared_lock rlock(map_mt);
        // проверяем, что данные присутствуют в кэше
        auto itr = cache_map.find(key);
        if (itr != cache_map.end()) {
            ++stat.read_count;
            return itr->second;
        }
        // сюда попадают потоки-читатели, которые не получили обещанных данных, например, из-за внешней процедуры
        // очистки кэша, пользователю придется повторить запрос
        ++stat.null_count;
        return nullptr;
    }

    void on_ready_data(KeyType const& key, UserType *result)
    {
        // блокируем доступ до точки А и вносим данные в кэш
        {
            std::unique_lock wlock(map_mt);
            cache_map.insert({key, std::shared_ptr<UserType>(result)});
            ++stat.write_count;
        }
        // сообщаем ждущим потокам-читателям, что запрос исполнен
        std::unique_lock req_lock(req_mt);
        active_requests.erase(key);
        // будим все ждущие потоки в точке C
        req_cv.notify_all();
    }

    ~Cache_line() {
        std::cout << "Cache statistics:" << std::endl
                << "hit_count: " << stat.hit_count << std::endl
                << "miss_count: " << stat.miss_count << std::endl
                << "read_count: " << stat.read_count << std::endl
                << "write_count: " << stat.write_count << std::endl
                << "null_count: " << stat.null_count << std::endl;
        //assert(stat.null_count + stat.read_count + stat.write_count == stat.miss_count);
    }

private:
    std::unordered_map<KeyType, Cache_value> cache_map;
    std::shared_mutex map_mt;

    std::unordered_set<KeyType> active_requests;
    std::mutex req_mt;
    std::condition_variable req_cv;

    struct Statistics {
        std::atomic_uint64_t hit_count;     // кол-во попаданий в кэш
        std::atomic_uint64_t miss_count;    // кол-во промахов мимо кэша
        std::atomic_uint64_t write_count;   // кол-во записей в кэш
        std::atomic_uint64_t read_count;    // кол-во успешных попыток чтения из кэша
        std::atomic_uint64_t null_count;    // кол-во неуспешных попыток чтения из кэша
    };
    Statistics stat;
};

// Служебная функция, извлекающая данные из источника
void writer(std::function<void (int *)> const& callback, int min, int max)
{
    callback(new int(min + std::rand()/((RAND_MAX + 1u)/(max-min))));
}

// Пользовательская функция, нуждающаяся в кэшированных данных
template <typename KeyType, typename UserType>
void reader(Cache_line<KeyType, UserType> &cache_line, KeyType min, KeyType max, size_t count, UserType vmin, UserType vmax)
{
    for(size_t i=0; i<count; ++i) {
        for (KeyType v = min; v < max; ++v) {
            typename Cache_line<KeyType, UserType>::Cache_value result;
            //нужен цикл из-за асинхронного вызова reset()
            do {
                result = cache_line.get_data(v, std::bind(&writer, std::placeholders::_1, vmin, vmax));
            } while(result == nullptr);
        }
    }
}

// Асинхронная по отношению к потокам чтения, процедура сброса кэша
template <typename KeyType, typename UserType>
void reset(Cache_line<KeyType, UserType> &cache_line, size_t count)
{
    for(size_t i=0; i<count; ++i) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(int(1 + std::rand()/((RAND_MAX + 1u)/499))));
        cache_line.reset();
    }
}

int main() {
    enum {
        KEY_MIN = 1,
        KEY_MAX = 100,
        COUNT = 1000,
        READ_THREAD_COUNT = 20,
        RESET_THREAD_COUNT = 2,
    };

    std::cout << "Hello, parallel World!" << std::endl;

    Cache_line<int, int> icache;
    std::vector<std::thread> threads;
    for(int i = 0; i<RESET_THREAD_COUNT; ++i) {
        threads.emplace_back(&reset<int, int>, std::ref(icache), COUNT);
    }
    for(int i = 0; i<READ_THREAD_COUNT; ++i) {
        threads.emplace_back(&reader<int, int>, std::ref(icache), KEY_MIN, KEY_MAX, COUNT, 1, 99);
    }
    for(auto &item: threads) {
        if(item.joinable())
            item.join();
    }
}
