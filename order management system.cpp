#include <iostream>
#include <string>
#include <queue>
#include <chrono>
#include <map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <fstream>
#include <atomic>
#include <algorithm>
#include <iomanip>

// Assuming these are provided or defined elsewhere
// struct Logon, Logout, OrderRequest, OrderResponse
// enum class RequestType, ResponseType


struct Logon
{
    std::string username;
    std::string password;
};

struct Logout
{
    std::string username;
};
enum class RequestType
{
    Unknown = 0,
    New = 1,
    Modify = 2,
    Cancel = 3
};

struct OrderRequest
{
    int m_symbolId;
    double m_price;
    uint64_t m_qty;
    char m_side;
    uint64_t m_orderId;
    RequestType m_requestType;                                  
    std::chrono::high_resolution_clock::time_point m_timestamp; 
};

enum class ResponseType
{
    Unknown = 0,
    Accept = 1,
    Reject = 2,
};

struct OrderResponse
{
    uint64_t m_orderId;
    ResponseType m_responseType;
};

// Helper struct to store queued orders with their original request type
struct QueuedOrder
{
    OrderRequest request;
    RequestType originalRequestType;
};

struct OrderLogEntry
{
    uint64_t orderId;
    ResponseType responseType;
    long long roundTripLatencyUs;
};

namespace Config
{
    int TRADE_START_HOUR = 10;
    int TRADE_START_MINUTE = 0;
    int TRADE_END_HOUR = 13;
    int TRADE_END_MINUTE = 0;
    int MAX_ORDERS_PER_SECOND = 100;
    const std::string LOG_FILE_PATH = "order_responses.log";
}

class OrderManagement
{
public:
    OrderManagement()
        : m_loggedIn(false),
          m_ordersSentThisSecond(0),
          m_lastSecondStart(std::chrono::high_resolution_clock::now())
    {
        m_timeMonitorThread = std::thread(&OrderManagement::timeWindowMonitor, this);
        m_orderSenderThread = std::thread(&OrderManagement::orderSender, this);
    }

    ~OrderManagement()
    {
        m_stopThreads.store(true);
        if (m_timeMonitorThread.joinable())
        {
            m_timeMonitorThread.join();
        }
        if (m_orderSenderThread.joinable())
        {
            m_orderSenderThread.join();
        }
    }

    void onData(OrderRequest &&request)
    {
        request.m_timestamp = std::chrono::high_resolution_clock::now();

        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_loggedIn.load())
        {
            std::cout << "Rejecting order " << request.m_orderId << ": Outside trading hours." << std::endl;

            return;
        }

        auto it = std::find_if(m_orderQueue.begin(), m_orderQueue.end(),[&](const QueuedOrder &qo)
                               { return qo.request.m_orderId == request.m_orderId; });

        if (it != m_orderQueue.end())
        {
            if (request.m_requestType == RequestType::Modify)
            {
                std::cout << "Modifying order " << request.m_orderId << " in queue." << std::endl;
                it->request.m_price = request.m_price;
                it->request.m_qty = request.m_qty;

                return; 
            }
            else if (request.m_requestType == RequestType::Cancel)
            {
                std::cout << "Cancelling order " << request.m_orderId << " from queue." << std::endl;
                m_orderQueue.erase(it);
                return; 
            }
        }


        if (request.m_requestType == RequestType::New ||
            (request.m_requestType == RequestType::Modify && it == m_orderQueue.end()) ||
            (request.m_requestType == RequestType::Cancel && it == m_orderQueue.end()))
        {
            QueuedOrder qo;
            qo.request = std::move(request);
            qo.originalRequestType = qo.request.m_requestType; 
            m_orderQueue.push_back(qo);
            m_cv.notify_one();
        }
    }

    void onData(OrderResponse &&response)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_pendingOrders.find(response.m_orderId);
        if (it != m_pendingOrders.end())
        {
            auto sendTime = it->second;
            auto receiveTime = std::chrono::high_resolution_clock::now();
            auto latency = std::chrono::duration_cast<std::chrono::microseconds>(receiveTime - sendTime).count();

            OrderLogEntry logEntry;
            logEntry.orderId = response.m_orderId;
            logEntry.responseType = response.m_responseType;
            logEntry.roundTripLatencyUs = latency;

            logOrderResponse(logEntry);

            std::cout << "Order Response - Order ID: " << response.m_orderId
                      << ", Type: " << static_cast<int>(response.m_responseType)
                      << ", Latency: " << latency << " us" << std::endl;

            m_pendingOrders.erase(it);
        }
        else
        {
            std::cout << "Received response for unknown order ID: " << response.m_orderId << std::endl;
        }
    }

    void send(const OrderRequest &request)
    {
    }

    void sendLogon()
    {
        std::cout << "Sending Logon message to exchange." << std::endl;
        m_loggedIn.store(true);
    }

    void sendLogout()
    {
        std::cout << "Sending Logout message to exchange." << std::endl;
        m_loggedIn.store(false);
    }

private:
    std::deque<QueuedOrder> m_orderQueue; 
    std::mutex m_mutex;
    std::condition_variable m_cv; 
    std::atomic<bool> m_loggedIn;

    int m_ordersSentThisSecond;
    std::chrono::high_resolution_clock::time_point m_lastSecondStart;

    std::map<uint64_t, std::chrono::high_resolution_clock::time_point> m_pendingOrders;

    std::thread m_timeMonitorThread;
    std::thread m_orderSenderThread;
    std::atomic<bool> m_stopThreads = false;

    void timeWindowMonitor()
    {
        while (!m_stopThreads.load())
        {
            auto now = std::chrono::system_clock::now();
            auto in_time_t = std::chrono::system_clock::to_time_t(now);
            std::tm tm_buf;
#ifdef _WIN32
            localtime_s(&tm_buf, &in_time_t);
#else
            localtime_r(&in_time_t, &tm_buf);
#endif

            int currentHour = tm_buf.tm_hour;
            int currentMinute = tm_buf.tm_min;

            bool shouldBeLoggedIn = false;
            if (Config::TRADE_START_HOUR < Config::TRADE_END_HOUR)
            {
                shouldBeLoggedIn = (currentHour > Config::TRADE_START_HOUR ||
                                    (currentHour == Config::TRADE_START_HOUR && currentMinute >= Config::TRADE_START_MINUTE)) &&
                                   (currentHour < Config::TRADE_END_HOUR ||
                                    (currentHour == Config::TRADE_END_HOUR && currentMinute < Config::TRADE_END_MINUTE));
            }
            else if (Config::TRADE_START_HOUR > Config::TRADE_END_HOUR)
            {
                shouldBeLoggedIn = (currentHour > Config::TRADE_START_HOUR ||
                                    (currentHour == Config::TRADE_START_HOUR && currentMinute >= Config::TRADE_START_MINUTE)) ||
                                   (currentHour < Config::TRADE_END_HOUR ||
                                    (currentHour == Config::TRADE_END_HOUR && currentMinute < Config::TRADE_END_MINUTE));
            }
            else
            {
                shouldBeLoggedIn = (currentHour == Config::TRADE_START_HOUR &&
                                    currentMinute >= Config::TRADE_START_MINUTE &&
                                    currentMinute < Config::TRADE_END_MINUTE);
            }

            if (shouldBeLoggedIn && !m_loggedIn.load())
            {
                sendLogon();
            }
            else if (!shouldBeLoggedIn && m_loggedIn.load())
            {
                sendLogout();
            }

            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }

    void orderSender()
    {
        while (!m_stopThreads.load())
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]
                      { return m_stopThreads.load() || (!m_orderQueue.empty() && m_loggedIn.load()); });

            if (m_stopThreads.load())
            {
                break;
            }

            if (!m_orderQueue.empty() && m_loggedIn.load())
            {
                auto now = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastSecondStart).count();

                if (elapsed >= 1)
                {
                    m_ordersSentThisSecond = 0;
                    m_lastSecondStart = now;
                }

                if (m_ordersSentThisSecond < Config::MAX_ORDERS_PER_SECOND)
                {
                    QueuedOrder queuedOrder = m_orderQueue.front();
                    m_orderQueue.pop_front();

                    lock.unlock();

                    // Record send time
                    m_pendingOrders[queuedOrder.request.m_orderId] = std::chrono::high_resolution_clock::now();

                    send(queuedOrder.request); 
                    m_ordersSentThisSecond++;

                    lock.lock();
                }
                else
                {
                    auto timeToWait = std::chrono::seconds(1) - (now - m_lastSecondStart);
                    if (timeToWait.count() > 0)
                    {
                        m_cv.wait_for(lock, timeToWait);
                    }
                }
            }
            else if (!m_loggedIn.load())
            {
                m_cv.wait_for(lock, std::chrono::seconds(1));
            }
        }
    }

    void logOrderResponse(const OrderLogEntry &entry)
    {
        std::ofstream ofs(Config::LOG_FILE_PATH, std::ios_base::app); // Open in append mode
        if (ofs.is_open())
        {
            ofs << "OrderID: " << entry.orderId
                << ", ResponseType: " << static_cast<int>(entry.responseType)
                << ", Latency(us): " << entry.roundTripLatencyUs << std::endl;
            ofs.close();
        }
        else
        {
            std::cerr << "Error: Could not open log file " << Config::LOG_FILE_PATH << std::endl;
        }
    }
};

int main()
{
    OrderManagement om;
    std::cout << "Starting simulation..." << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "\nSimulating order reception..." << std::endl;

    for (int i = 0; i < Config::MAX_ORDERS_PER_SECOND + 5; ++i)
    {
        OrderRequest req;
        req.m_orderId = 1000 + i;
        req.m_symbolId = 123;
        req.m_price = 100.50 + i;
        req.m_qty = 100 + i;
        req.m_side = (i % 2 == 0) ? 'B' : 'S';
        req.m_requestType = RequestType::New;
        om.onData(std::move(req));

        if (i < Config::MAX_ORDERS_PER_SECOND)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    OrderRequest modifyReq;
    modifyReq.m_orderId = 1000 + Config::MAX_ORDERS_PER_SECOND + 2;
    modifyReq.m_symbolId = 123;                                     
    modifyReq.m_price = 200.00;                                     
    modifyReq.m_qty = 50;                                           
    modifyReq.m_side = 'B';                                         
    modifyReq.m_requestType = RequestType::Modify;
    std::cout << "\nSimulating a modify order for an order in queue..." << std::endl;
    om.onData(std::move(modifyReq));

    OrderRequest cancelReq;
    cancelReq.m_orderId = 1000 + Config::MAX_ORDERS_PER_SECOND + 1; 
    cancelReq.m_symbolId = 123;                                     
    cancelReq.m_price = 0.0;                                        
    cancelReq.m_qty = 0;                                            
    cancelReq.m_side = 'X';                                         
    cancelReq.m_requestType = RequestType::Cancel;
    std::cout << "\nSimulating a cancel order for an order in queue..." << std::endl;
    om.onData(std::move(cancelReq));

    std::cout << "\nSimulating order responses..." << std::endl;
    for (int i = 0; i < Config::MAX_ORDERS_PER_SECOND + 5; ++i)
    {
        if (1000 + i == 1000 + Config::MAX_ORDERS_PER_SECOND + 1)
        {
            continue;
        }

        OrderResponse res;
        res.m_orderId = 1000 + i;
        res.m_responseType = (i % 3 == 0) ? ResponseType::Reject : ResponseType::Accept;
        om.onData(std::move(res));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "\nSimulation complete. Check order_responses.log for output." << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(5));

    return 0;
}