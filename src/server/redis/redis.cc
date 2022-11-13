#include <iostream>

#include "redis.hpp"

Redis::Redis()
    : _publish_context(nullptr), _subcribe_context(nullptr) // 上下文指针
{
}

Redis::~Redis()
{
    if (_publish_context != nullptr)
    {
        redisFree(_publish_context);
    }

    if (_subcribe_context != nullptr)
    {
        redisFree(_subcribe_context);
    }
}

// 连接 redis 服务器
bool Redis::connect()
{
    // 负责 publish 发布消息的上下文连接
    _publish_context = redisConnect("127.0.0.1", 6379);
    if (nullptr == _publish_context)
    {
        std::cerr << "connect redis failed!\n"
                  << std::endl;
        return false;
    }

    // 负责 subscribe 订阅消息的上下文连接
    _subcribe_context = redisConnect("127.0.0.1", 6379);
    if (nullptr == _subcribe_context)
    {
        std::cerr << "connect redis failed!\n"
                  << std::endl;
        return false;
    }

    // 在单独的线程中，监听通道上的事件，有消息给业务层进行上报
    std::thread t([&]() -> void
                  { observer_channel_message(); });

    t.detach();

    std::cout << "connect redis-server success!\n"
              << std::endl;

    return true;
}

// 向 redis 指定的通道 channel 发布消息
// redisCommand 先把命令缓存在本地，然后把命令发送到 redis-server，然后阻塞等待命令的执行结果
bool Redis::publish(int channel, std::string message)
{
    redisReply *reply = (redisReply *)redisCommand(this->_publish_context, "PUBLISH %d %s", channel, message.c_str());
    if (nullptr == reply)
    {
        std::cerr << "channel: " << channel << "publish command failed!\n"
                  << std::endl;
        return false;
    }
    freeReplyObject(reply);
    return true;
}

// 向 redis 指定的通道 subscribe 订阅消息
bool Redis::subscribe(int channel)
{
    // SUBSCRIBE 命令本身会造成线程阻塞等待通道里面发生消息，这里只做订阅通道，不接收通道消息
    // 通道消息的接收专门在 observer_channel_message 函数中的独立线程中进行
    // 只负责发送命令，不阻塞接收 redis server 响应消息，否则和 notifyMsg 线程抢占响应资源
    if (REDIS_ERR == redisAppendCommand(this->_subcribe_context, "SUBSCRIBE %d", channel))
    {
        std::cerr << "channel: " << channel << ", subscribe command failed!\n"
                  << std::endl;
        return false;
    }

    // redisBufferWrite 可以循环发送缓冲区，直到缓冲区数据发送完毕（ done 被置为1 ）
    int done = 0;
    while (!done)
    {
        if (REDIS_ERR == redisBufferWrite(this->_subcribe_context, &done))
        {
            std::cerr << "channel: " << channel << "subscribe command failed!\n"
                      << std::endl;
            return false;
        }
    }
    // redisGetReply

    return true;
}

// 向 redis 指定的通道 unsubscribe 取消订阅消息
bool Redis::unsubscribe(int channel)
{
    if (REDIS_ERR == redisAppendCommand(this->_subcribe_context, "UNSUBSCRIBE %d", channel))
    {
        std::cerr << "channel: " << channel << "unsubscribe command failed!\n"
                  << std::endl;
        return false;
    }

    // redisBufferWrite 可以循环发送缓冲区，直到缓冲区数据发送完毕（ done被置为1 ）
    int done = 0;
    while (!done)
    {
        if (REDIS_ERR == redisBufferWrite(this->_subcribe_context, &done))
        {
            std::cerr << "channel: " << channel << "unsubscribe command failed!\n"
                      << std::endl;
            return false;
        }
    }
    return true;
}

// 在独立线程中接收订阅通道中的消息
void Redis::observer_channel_message()
{
    redisReply *reply = nullptr;
    while (REDIS_OK == redisGetReply(this->_subcribe_context, (void **)&reply))
    {
        // 订阅收到的消息是一个带三元素的数组
        if (reply != nullptr && reply->element[2] != nullptr && reply->element[2]->str != nullptr)
        {
            // 给业务层上报通道上发生的消息
            _notify_message_handler(std::atoi(reply->element[1]->str), reply->element[2]->str);

            freeReplyObject(reply);
        }
    }
    std::cerr << ">>>>>>>>>>>>> observer_channel_message quit <<<<<<<<<<<<<\n"
              << std::endl;
}

// 初始化向业务层上报通道消息的回调对象
void Redis::init_notify_handler(std::function<void(int, std::string)> fn)
{
    this->_notify_message_handler = fn;
}
