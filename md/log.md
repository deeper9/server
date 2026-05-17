# 各个类的职责
## LogEvent
职责：
数据的载体。每次打印日志时，都会生成一个 LogEvent 对象，里面打包了这条日志的所有上下文信息（文件名、行号、线程ID、协程ID、时间戳、日志级别、日志器指针）以及具体的日志内容（存放在内部的 std::stringstream m_ss 中）。

## LogEventWrap
职责：
利用 RAII 机制（资源获取即初始化）触发日志写入。它的构造函数接收 LogEvent，并提供 getSS() 供外部使用 << 流式写入内容。最关键的是它的析构函数，当 LogEventWrap 对象生命周期结束时（通常是宏展开的单行语句结束时），它的析构函数会被调用，里面执行了 m_event->getLogger()->log(...)，正式将事件推给 Logger 处理

## LogFormatter
职责：决定日志长什么样。它包含一个 FormatItem 的集合，负责将 LogEvent 中的各个字段（时间、线程、内容等）按照预设的模板（如 %d %m %n）拼接成最终的字符串

## Logger
职责：日志系统的枢纽。它包含一个日志级别（m_level），用于过滤低于该级别的日志。如果日志级别达标，它会将 LogEvent 分发给它所拥有的所有 LogAppender。如果它自己没有配置 LogAppender，它会尝试将事件抛给它的父节点（通常是 m_root 主日志器）。

# 执行流程
假设有代码 ```SYLAR_LOG_INFO(g_logger) << "hello sylar log";```
1. 展开宏
``` C++
if(g_logger->getLevel() <= sylar::LogLevel::INFO)
    sylar::LogEventWrap(
        sylar::LogEvent::ptr(new sylar::LogEvent(g_logger, sylar::LogLevel::INFO, __FILE__, __LINE__, ...))
    ).getSS() << "hello sylar log";
```
* 判断级别：首先判断当前 g_logger 的级别是否允许输出 INFO 日志。
* 创建事件：如果允许，new 一个 LogEvent，把当前的文件名、行号、线程ID等元数据塞进去。
* 流式写入：创建一个临时的匿名 LogEventWrap 对象，调用 getSS() 获取底层的 stringstream，然后把 "hello sylar log" 写入到这个流中。
2. RAII 触发日志提交流程 (流转端)
``` C++
LogEventWrap::~LogEventWrap() {
    m_event->getLogger()->log(m_event->getLevel(), m_event);
}
```
* 打包好的 LogEvent 被正式提交给了 g_logger 的 log 方法。
3. Logger 分发事件 (枢纽端)
* ``` Logger::log ```里面逐个分发事件
4. Appender 处理与 Formatter 格式化 (输出端)
假设给了stdout
``` C++
void StdoutLogAppender::log(std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) {
    if(level >= m_level) {
        MutexType::Lock lock(m_mutex);
        // 调用 Formatter 进行格式化，并直接输出到 std::cout
        m_formatter->format(std::cout, logger, level, event);
    }
}
```
5. formatter里面在逐个item输出
``` C++
std::ostream& LogFormatter::format(std::ostream& ofs, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) {
    for(auto& i : m_items) {
        i->format(ofs, logger, level, event);
    }
    return ofs;
}
```
6. items里面挨个项输出到ofs中