#include <sdkddkver.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <iostream>
#include <vector>
#include <deque>
#include <string_view>
#include <chrono>
#include <thread>
#include <future>

// https://github.com/richgel999/fpng
#include "fpng.h"

// https://think-async.com/Asio/index.html
#define ASIO_STANDALONE
#include <asio.hpp>

static int32_t runClient(const std::string &serverIP, const std::string &serverPort);
static int32_t runServer(const std::string &serverPort);

using hires_clock = std::chrono::high_resolution_clock;

static hires_clock::time_point g_appStartTp;



int32_t main(int32_t argc, const char* argv[]) {
    // レンダラー起動時間を取得。
    g_appStartTp = hires_clock::now();

    std::string serverIP;
    std::string serverPort;
    bool isServerMode = true;
    for (int argIdx = 1; argIdx < argc; ++argIdx) {
        std::string_view arg = argv[argIdx];
        if (arg == "--client") {
            isServerMode = false;
            if (argIdx + 2 >= argc) {
                printf("--client requires a server IP and a port.\n");
                return -1;
            }
            serverIP = argv[argIdx + 1];
            serverPort = argv[argIdx + 2];
            argIdx += 2;
        }
        else if (arg == "--server") {
            isServerMode = true;
            if (argIdx + 1 >= argc) {
                printf("--server requires a port.\n");
                return -1;
            }
            serverPort = argv[argIdx + 1];
            argIdx += 1;
        }
        else {
            printf("Unknown argument %s.\n", argv[argIdx]);
            return -1;
        }
    }

    if (isServerMode) {
        if (serverPort.empty()) {
            printf("Specify a server port.\n");
            return -1;
        }
        printf("Run as a server.\n");
        runServer(serverPort);
    }
    else {
        printf("Run as a client.\n");
        runClient(serverIP, serverPort);
    }

    return 0;
}



enum class MessageType {
    SessionID = 0,
    ServerStateRequest,
    ServerState,
    RenderTaskRequest,
    RenderTask,
    FinishSignal,
};

struct MessageHeader {
    MessageType type;
    uint32_t dataLength;
};

enum class ServerState {
    PreparingData = 0,
    DataReady,
    Finishing,
    Unknown,
};

struct RenderTask {
    uint32_t frameIndex;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t isValid : 1;
};



class Client {
    asio::ip::tcp::socket m_socket;
    asio::ip::tcp::resolver::results_type m_endpoints;
    uint32_t m_maxNumConnectTrials;
    uint32_t m_numConnectTrials;
    uint32_t m_connectionRetryInterval;
    uint32_t m_sessionID;
    ServerState m_lastServerState;
    RenderTask m_lastRenderTask;
    std::vector<uint8_t> m_receivedData;
    std::vector<uint8_t> m_sentData;

    template <typename T>
    const T &getReceivedDataAs() const {
        return *reinterpret_cast<const T*>(m_receivedData.data());
    }

    template <typename T>
    void setSendData(const T &data) {
        m_sentData.resize(sizeof(data));
        std::memcpy(m_sentData.data(), &data, sizeof(data));
    }

    void registerConnect(const asio::ip::tcp::resolver::results_type &endpoints) {
        using asio::ip::tcp;

        asio::async_connect(
            m_socket, endpoints,
            [this](asio::error_code ec, const tcp::endpoint &ep) {
                if (ec) {
                    const tcp::endpoint &fep = *m_endpoints;
                    printf(
                        "Tried to connect to %s:%u (%u/%u)\n",
                        fep.address().to_string().c_str(), static_cast<uint32_t>(fep.port()),
                        m_numConnectTrials + 1, m_maxNumConnectTrials);
                    std::this_thread::sleep_for(std::chrono::milliseconds(m_connectionRetryInterval));
                    ++m_numConnectTrials;
                    if (m_numConnectTrials < m_maxNumConnectTrials) {
                        registerConnect(m_endpoints);
                    }
                    else {
                        char msg[128];
                        sprintf_s(msg, "Failed %u times.\n", m_maxNumConnectTrials);
                        throw std::runtime_error(msg);
                    }
                }
                else {
                    printf(
                        "Connected to %s:%u\n",
                        ep.address().to_string().c_str(), static_cast<uint32_t>(ep.port()));

                    // ヘッダー受信。
                    m_receivedData.resize(sizeof(MessageHeader));
                    asio::async_read(
                        m_socket,
                        asio::buffer(m_receivedData),
                        [this](asio::error_code ec, std::size_t length) {
                            const auto receivedHeader = getReceivedDataAs<MessageHeader>();
                            assert(receivedHeader.type == MessageType::SessionID);
                            assert(receivedHeader.dataLength == sizeof(uint32_t));

                            // セッションID受信。
                            m_receivedData.resize(receivedHeader.dataLength);
                            asio::async_read(
                                m_socket,
                                asio::buffer(m_receivedData),
                                [this](asio::error_code ec, std::size_t length) {
                                    m_sessionID = getReceivedDataAs<uint32_t>();
                                    registerCommunication();
                                });
                        });
                }
            });
    }

    void registerSendFinish() {
        // 終了シグナルを送る。
        MessageHeader header = {};
        header.type = MessageType::FinishSignal;
        setSendData(header);
        asio::async_write(
            m_socket,
            asio::buffer(m_sentData),
            [this](asio::error_code ec, std::size_t length) {
            });
    }

    void registerServerStateRequest() {
        // サーバー状態リクエスト。
        MessageHeader header = {};
        header.type = MessageType::ServerStateRequest;
        setSendData(header);
        asio::async_write(
            m_socket,
            asio::buffer(m_sentData),
            [this](asio::error_code ec, std::size_t length) {
                // ヘッダー受信。
                m_receivedData.resize(sizeof(MessageHeader));
                asio::async_read(
                    m_socket,
                    asio::buffer(m_receivedData),
                    [this](asio::error_code ec, std::size_t length) {
                        const auto receivedHeader = getReceivedDataAs<MessageHeader>();
                        assert(receivedHeader.type == MessageType::ServerState);

                        // サーバー状態受信。
                        m_receivedData.resize(receivedHeader.dataLength);
                        asio::async_read(
                            m_socket,
                            asio::buffer(m_receivedData),
                            [this](asio::error_code ec, std::size_t length) {
                                m_lastServerState = getReceivedDataAs<ServerState>();
                                if (m_lastServerState == ServerState::Finishing)
                                    registerSendFinish();
                                else
                                    registerCommunication();
                            });
                    });
            });
    }

    void registerCommunication() {
        using asio::ip::tcp;

        if (m_lastServerState == ServerState::Unknown ||
            m_lastServerState == ServerState::PreparingData) {
            registerServerStateRequest();
        }
        else if (m_lastServerState == ServerState::DataReady) {
            // レンダータスクリクエスト。
            MessageHeader header = {};
            header.type = MessageType::RenderTaskRequest;
            setSendData(header);
            asio::async_write(
                m_socket,
                asio::buffer(m_sentData),
                [this](asio::error_code ec, std::size_t length) {
                    // ヘッダー受信。
                    m_receivedData.resize(sizeof(MessageHeader));
                    asio::async_read(
                        m_socket,
                        asio::buffer(m_receivedData),
                        [this](asio::error_code ec, std::size_t length) {
                            const auto receivedHeader = getReceivedDataAs<MessageHeader>();
                            assert(receivedHeader.type == MessageType::RenderTask);

                            // レンダータスク受信。
                            m_receivedData.resize(receivedHeader.dataLength);
                            asio::async_read(
                                m_socket,
                                asio::buffer(m_receivedData),
                                [this](asio::error_code ec, std::size_t length) {
                                    m_lastRenderTask = getReceivedDataAs<RenderTask>();
                                    render();
                                });
                        });
                });
        }
    }

    void render() {
        if (m_lastRenderTask.isValid) {
            const uint32_t frameIndex = m_lastRenderTask.frameIndex;

            using namespace fpng;
            fpng_init();

            struct RGBA {
                uint32_t r : 8;
                uint32_t g : 8;
                uint32_t b : 8;
                uint32_t a : 8;
            };
            constexpr uint32_t width = 256;
            constexpr uint32_t height = 256;
            std::vector<RGBA> pixels(height * width);

            const hires_clock::time_point frameStartTp = hires_clock::now();
            printf("[%u]: Frame %u ... ", m_sessionID, frameIndex);

            // 高度なレンダリング...
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    RGBA v;
                    v.r = x;
                    v.g = y;
                    v.b = frameIndex;
                    v.a = 255;
                    const int32_t idx = y * width + x;
                    pixels[idx] = v;
                }
            }

            // 起動からの時刻とフレーム時間を計算。
            const hires_clock::time_point now = hires_clock::now();
            const hires_clock::duration frameTime = now - frameStartTp;
            const hires_clock::duration totalTime = now - g_appStartTp;
            printf(
                "Done: %.3f [ms] (total: %.3f [s])\n",
                std::chrono::duration_cast<std::chrono::microseconds>(frameTime).count() * 1e-3f,
                std::chrono::duration_cast<std::chrono::milliseconds>(totalTime).count() * 1e-3f);

            // 3桁連番で画像出力。
            char filename[256];
            sprintf_s(filename, "%03u.png", frameIndex);
            fpng_encode_image_to_file(filename, pixels.data(), width, height, 4, 0);
        }

        registerServerStateRequest();
    }

public:
    Client(
        asio::io_context &ioContext,
        const std::string &host, const std::string &port,
        uint32_t maxNumConnectTrials, uint32_t connectionRetryInterval) :
        m_socket(ioContext),
        m_maxNumConnectTrials(maxNumConnectTrials), m_numConnectTrials(0),
        m_connectionRetryInterval(connectionRetryInterval),
        m_lastServerState(ServerState::Unknown) {
        using asio::ip::tcp;

        tcp::resolver resolver(ioContext);
        m_endpoints = resolver.resolve(host, port);
        registerConnect(m_endpoints);
    }
};



int32_t runClient(const std::string &serverIP, const std::string &serverPort) {
    using asio::ip::tcp;

    try {
        printf("Start client.\n");
        asio::io_context ioContext;
        constexpr uint32_t maxNumConnectionTrials = 10;
        constexpr uint32_t connectionRetryInterval = 500;
        Client client(ioContext, serverIP, serverPort, maxNumConnectionTrials, connectionRetryInterval);
        ioContext.run();
        printf("Quit client.\n");
    }
    catch (std::exception& e) {
        printf("%s\n", e.what());
        return -1;
    }

    return 0;
}



class Server;

class Session : public std::enable_shared_from_this<Session> {
    Server &m_server;
    uint32_t m_ID;
    std::vector<uint8_t> m_receivedData;
    std::vector<uint8_t> m_sentData;
    asio::ip::tcp::socket m_socket;

    template <typename T>
    const T &getReceivedDataAs() const {
        return *reinterpret_cast<const T*>(m_receivedData.data());
    }

    template <typename T>
    void setSendData(const T &data) {
        m_sentData.resize(sizeof(data));
        std::memcpy(m_sentData.data(), &data, sizeof(data));
    }

    void registerCommunication();

public:
    Session(Server &server, uint32_t id, asio::ip::tcp::socket socket) :
        m_server(server), m_ID(id), m_socket(std::move(socket)) {
    }
    ~Session() {
        using asio::ip::tcp;
        const tcp::endpoint &clientEndpoint = m_socket.remote_endpoint();
        printf(
            "Quit the session with %s:%u.\n",
            clientEndpoint.address().to_string().c_str(),
            static_cast<uint32_t>(clientEndpoint.port()));
    }

    void start() {
        using asio::ip::tcp;
        const tcp::endpoint &clientEndpoint = m_socket.remote_endpoint();
        printf(
            "Start a session with %s:%u.\n",
            clientEndpoint.address().to_string().c_str(),
            static_cast<uint32_t>(clientEndpoint.port()));

        auto self(shared_from_this());
        // ヘッダー送信。
        MessageHeader header = {};
        header.type = MessageType::SessionID;
        header.dataLength = sizeof(m_ID);
        setSendData(header);
        asio::async_write(
            m_socket,
            asio::buffer(m_sentData),
            [this, self](asio::error_code ec, std::size_t length) {
                // セッションID送信。
                setSendData(m_ID);
                asio::async_write(
                    m_socket,
                    asio::buffer(m_sentData),
                    [this, self](asio::error_code ec, std::size_t length) {
                        registerCommunication();
                    });
            });
    }
};



class Server {
    ServerState m_state;
    asio::ip::tcp::acceptor m_acceptor;
    std::deque<RenderTask> m_renderTasks;
    uint32_t m_nextSessionID;

    void registerAccept() {
        using asio::ip::tcp;

        m_acceptor.async_accept(
            [this](asio::error_code ec, tcp::socket socket) {
                if (!ec)
                    std::make_shared<Session>(*this, m_nextSessionID++, std::move(socket))->start();
                if (ec != asio::error::operation_aborted)
                    registerAccept();
            });
    }

public:
    Server(asio::io_context &ioContext, uint32_t port) :
        m_state(ServerState::PreparingData),
        m_acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)),
        m_nextSessionID(0) {
        registerAccept();

        m_renderTasks.resize(256);
        for (int i = 0; i < 256; ++i) {
            RenderTask task = {};
            task.frameIndex = i;
            task.x = 0;
            task.y = 0;
            task.width = 256;
            task.height = 256;
            task.isValid = true;
            m_renderTasks.push_back(task);
        }
        m_state = ServerState::DataReady;
    }

    ServerState getState() const {
        return m_state;
    }

    RenderTask popRenderTask() {
        RenderTask task = {};
        if (m_renderTasks.empty()) {
            m_state = ServerState::Finishing;
            m_acceptor.cancel();
        }
        else {
            task = m_renderTasks.front();
            m_renderTasks.pop_front();
        }
        return task;
    }
};



void Session::registerCommunication() {
    using asio::ip::tcp;

    auto self(shared_from_this());
    m_receivedData.resize(sizeof(MessageHeader));
    asio::async_read(
        m_socket,
        asio::buffer(m_receivedData),
        [this, self](asio::error_code ec, std::size_t length) {
            if (ec)
                return;

            const auto receivedHeader = getReceivedDataAs<MessageHeader>();

            if (receivedHeader.type == MessageType::ServerStateRequest) {
                assert(receivedHeader.dataLength == 0);

                const ServerState state = m_server.getState();

                // ヘッダー送信。
                MessageHeader header = {};
                header.type = MessageType::ServerState;
                header.dataLength = sizeof(state);
                setSendData(header);
                asio::async_write(
                    m_socket,
                    asio::buffer(m_sentData),
                    [this, self, state](asio::error_code ec, std::size_t length) {
                        // サーバー状態送信。
                        setSendData(state);
                        asio::async_write(
                            m_socket,
                            asio::buffer(m_sentData),
                            [this, self](asio::error_code ec, std::size_t length) {
                                registerCommunication();
                            });
                    });
            }
            else if (receivedHeader.type == MessageType::RenderTaskRequest) {
                assert(receivedHeader.dataLength == 0);

                const RenderTask task = m_server.popRenderTask();

                // ヘッダー送信。
                MessageHeader header = {};
                header.type = MessageType::RenderTask;
                header.dataLength = sizeof(task);
                setSendData(header);
                asio::async_write(
                    m_socket,
                    asio::buffer(m_sentData),
                    [this, self, task](asio::error_code ec, std::size_t length) {
                        // レンダータスク送信。
                        setSendData(task);
                        asio::async_write(
                            m_socket,
                            asio::buffer(m_sentData),
                            [this, self](asio::error_code ec, std::size_t length) {
                                registerCommunication();
                            });
                    });
            }
            else if (receivedHeader.type == MessageType::FinishSignal) {
                assert(receivedHeader.dataLength == 0);
            }
            else {
                assert(false);
            }
        });
}



void runLocalClient(std::promise<int32_t> &ret, const std::string &serverPort) {
    ret.set_value(runClient("127.0.0.1", serverPort));
}



int32_t runServer(const std::string &serverPort) {
    using asio::ip::tcp;

    try {
        printf("Start server.\n");
        asio::io_context ioContext;
        Server server(ioContext, static_cast<uint32_t>(atoi(serverPort.c_str())));

        // サーバーPCもクライアントとしてのスレッドを起動する。
        std::promise<int32_t> promLocalClient;
        std::future<int32_t> futLocalClient = promLocalClient.get_future();
        std::thread localClient(runLocalClient, std::ref(promLocalClient), serverPort);

        ioContext.run();
        printf("Quit server.\n");

        if (futLocalClient.get() != 0)
            throw std::runtime_error("Something went wrong in the local client.");
        localClient.join();
    }
    catch (std::exception &e) {
        printf("%s\n", e.what());
        return -1;
    }

    return 0;
}
