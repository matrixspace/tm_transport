#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <cppzmq/zmq.hpp>

#include <tm_kit/transport/zeromq/ZeroMQComponent.hpp>

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace zeromq {
    class ZeroMQComponentImpl {
    private:
        class OneZeroMQSubscription {
        private:
            std::array<char, 16*1024*1024> buffer_;
            struct ClientCB {
                std::function<void(basic::ByteDataWithTopic &&)> cb;
                std::optional<WireToUserHook> hook;
            };
            std::vector<ClientCB> noFilterClients_;
            std::vector<std::tuple<std::string, ClientCB>> stringMatchClients_;
            std::vector<std::tuple<std::regex, ClientCB>> regexMatchClients_;
            std::mutex mutex_;
            std::thread th_;
            std::atomic<bool> running_;

            inline void callClient(ClientCB const &c, basic::ByteDataWithTopic &&d) {
                if (c.hook) {
                    auto b = (c.hook->hook)(basic::ByteData {std::move(d.content)});
                    if (b) {
                        c.cb({std::move(d.topic), std::move(b->content)});
                    }
                } else {
                    c.cb(std::move(d));
                }
            }

            void run(ConnectionLocator const &locator, zmq::context_t *p_ctx) {
                zmq::socket_t sock(*p_ctx, zmq::socket_type::sub);
                sock.set(zmq::sockopt::rcvtimeo, 1000);

                std::ostringstream oss;
                oss << "tcp://" << locator.host() << ":" << locator.port();
                sock.connect(oss.str());
                sock.set(zmq::sockopt::subscribe, "");

                while (running_) {
                    auto res = sock.recv(
                        zmq::mutable_buffer(buffer_.data(), 16*1024*1024)
                    );
                    if (!res) {
                        continue;
                    }
                    if (res->truncated()) {
                        continue;
                    }
                    
                    auto parseRes = basic::bytedata_utils::RunCBORDeserializer<basic::ByteDataWithTopic>::apply(std::string_view {buffer_.data(), res->size}, 0);
                    if (parseRes && std::get<1>(*parseRes) == res->size) {
                        basic::ByteDataWithTopic data = std::move(std::get<0>(*parseRes));

                        std::lock_guard<std::mutex> _(mutex_);
                        
                        for (auto const &f : noFilterClients_) {
                            callClient(f, basic::ByteDataWithTopic {data});
                        }
                        for (auto const &f : stringMatchClients_) {
                            if (data.topic == std::get<0>(f)) {
                                callClient(std::get<1>(f), basic::ByteDataWithTopic {data});
                            }
                        }
                        for (auto const &f : regexMatchClients_) {
                            if (std::regex_match(data.topic, std::get<0>(f))) {
                                callClient(std::get<1>(f), basic::ByteDataWithTopic {data});
                            }
                        }
                    }  
                }
            }
        public:
            OneZeroMQSubscription(ConnectionLocator const &locator, zmq::context_t *p_ctx) 
                : buffer_()
                , noFilterClients_(), stringMatchClients_(), regexMatchClients_()
                , mutex_(), th_(), running_(true)
            {
                th_ = std::thread(&OneZeroMQSubscription::run, this, locator, p_ctx);
            }
            ~OneZeroMQSubscription() {
                if (running_) {
                    running_ = false;
                    th_.join();
                }
            }
            void addSubscription(
                std::variant<ZeroMQComponent::NoTopicSelection, std::string, std::regex> const &topic
                , std::function<void(basic::ByteDataWithTopic &&)> handler
                , std::optional<WireToUserHook> wireToUserHook
            ) {
                std::lock_guard<std::mutex> _(mutex_);
                switch (topic.index()) {
                case 0:
                    noFilterClients_.push_back({handler, wireToUserHook});
                    break;
                case 1:
                    stringMatchClients_.push_back({std::get<std::string>(topic), {handler, wireToUserHook}});
                    break;
                case 2:
                    regexMatchClients_.push_back({std::get<std::regex>(topic), {handler, wireToUserHook}});
                    break;
                default:
                    break;
                }
            }
        };
        std::unordered_map<ConnectionLocator, std::unique_ptr<OneZeroMQSubscription>> subscriptions_;
        
        class OneZeroMQSender {
        private:
            std::thread thread_;
            std::list<basic::ByteDataWithTopic> incoming_, processing_;
            std::mutex mutex_;
            std::condition_variable cond_;
            std::atomic<bool> running_;
            void run(int port, zmq::context_t *p_ctx) {
                zmq::socket_t sock(*p_ctx, zmq::socket_type::pub);
                std::ostringstream oss;
                oss << "tcp://*:" << port;
                sock.bind(oss.str());
                while (running_) {
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cond_.wait_for(lock, std::chrono::milliseconds(1));
                        if (!running_) {
                            lock.unlock();
                            break;
                        }
                        if (incoming_.empty()) {
                            lock.unlock();
                            continue;
                        }
                        processing_.splice(processing_.end(), incoming_);
                        lock.unlock();
                    }
                    while (!processing_.empty()) {
                        auto const &item = processing_.front();
                        auto v = basic::bytedata_utils::RunCBORSerializer<basic::ByteDataWithTopic>::apply(item);

                        sock.send(
                            zmq::const_buffer(reinterpret_cast<char const *>(v.data()), v.size())
                            , zmq::send_flags::dontwait
                        );   

                        processing_.pop_front();
                    }
                }
            }
        public:
            OneZeroMQSender(int port, zmq::context_t *p_ctx)
                : thread_(), incoming_(), processing_(), mutex_()
                , cond_(), running_(true)
            {
                thread_ = std::thread(&OneZeroMQSender::run, this, port, p_ctx);
            }
            ~OneZeroMQSender() {
                running_ = false;
                thread_.join();
            }
            void publish(basic::ByteDataWithTopic &&data) {
                {
                    std::lock_guard<std::mutex> _(mutex_);
                    incoming_.push_back(std::move(data));
                }
                cond_.notify_one();          
            }
        };

        std::unordered_map<int, std::unique_ptr<OneZeroMQSender>> senders_;

        std::mutex mutex_;
        zmq::context_t ctx_;

        OneZeroMQSubscription *getOrStartSubscription(ConnectionLocator const &d) {
            ConnectionLocator hostAndPort {d.host(), d.port()};
            std::lock_guard<std::mutex> _(mutex_);
            auto iter = subscriptions_.find(hostAndPort);
            if (iter == subscriptions_.end()) {
                iter = subscriptions_.insert({hostAndPort, std::make_unique<OneZeroMQSubscription>(hostAndPort, &ctx_)}).first;
            }
            return iter->second.get();
        }
        OneZeroMQSender *getOrStartSender(ConnectionLocator const &d) {
            std::lock_guard<std::mutex> _(mutex_);
            auto iter = senders_.find(d.port());
            if (iter == senders_.end()) {
                iter = senders_.insert({d.port(), std::make_unique<OneZeroMQSender>(d.port(), &ctx_)}).first;
            }
            return iter->second.get();
        }
    public:
        ZeroMQComponentImpl()
            : subscriptions_(), senders_(), mutex_(), ctx_() {            
        }
        ~ZeroMQComponentImpl() = default;
        void addSubscriptionClient(ConnectionLocator const &locator,
            std::variant<ZeroMQComponent::NoTopicSelection, std::string, std::regex> const &topic,
            std::function<void(basic::ByteDataWithTopic &&)> client,
            std::optional<WireToUserHook> wireToUserHook) {
            auto *p = getOrStartSubscription(locator);
            p->addSubscription(topic, client, wireToUserHook);
        }
        std::function<void(basic::ByteDataWithTopic &&)> getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook) {
            auto *p = getOrStartSender(locator);
            if (userToWireHook) {
                auto hook = userToWireHook->hook;
                return [p,hook](basic::ByteDataWithTopic &&data) {
                    auto w = hook(basic::ByteData {std::move(data.content)});
                    p->publish({std::move(data.topic), std::move(w.content)});
                };
            } else {
                return [p](basic::ByteDataWithTopic &&data) {
                    p->publish(std::move(data));
                };
            }
        }
    };

    ZeroMQComponent::ZeroMQComponent() : impl_(std::make_unique<ZeroMQComponentImpl>()) {}
    ZeroMQComponent::~ZeroMQComponent() {}
    void ZeroMQComponent::zeroMQ_addSubscriptionClient(ConnectionLocator const &locator,
        std::variant<ZeroMQComponent::NoTopicSelection, std::string, std::regex> const &topic,
        std::function<void(basic::ByteDataWithTopic &&)> client,
        std::optional<WireToUserHook> wireToUserHook) {
        impl_->addSubscriptionClient(locator, topic, client, wireToUserHook);
    }
    std::function<void(basic::ByteDataWithTopic &&)> ZeroMQComponent::zeroMQ_getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook) {
        return impl_->getPublisher(locator, userToWireHook);
    }
} } } } }