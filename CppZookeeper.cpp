#include "CppZookeeper.h"

#include <unistd.h>
#include <sys/syscall.h>
#include <arpa/inet.h>

#include <cstring>

#ifdef CPP_ZK_USE_BOOST
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#endif

#define ERR_LOG(a,b,format,...)    fprintf(stderr, format,##__VA_ARGS__)
#define WARN_LOG(a,b,format,...)   fprintf(stderr, format,##__VA_ARGS__)
#define INFO_LOG(a,b,format,...)   fprintf(stdout, format,##__VA_ARGS__)
#define DEBUG_LOG(a,b,format,...)  fprintf(stdout, format,##__VA_ARGS__)

// hashtable_search需要包含
#include <hashtable/hashtable_private.h>

// 这个宏必须加上，因为封装API基于多线程，多线程和单线程的对象是不一样的，二者不能共用
#define THREADED
#include <zk_adaptor.h>

// 用于删除注册的Watcher，数据结构从zk_hashtable.c中获得
typedef struct _watcher_object
{
    watcher_fn watcher;
    void* context;
    struct _watcher_object* next;
} watcher_object_t;

struct _zk_hashtable
{
    struct hashtable* ht;
};

struct watcher_object_list
{
    watcher_object_t* head;
};

using namespace std;

namespace zookeeper
{

void SplitStr(string str, const vector<string> &splitStr, vector<string> &result, bool removeEmptyElm = true, size_t maxCount = 0)
{
    result.clear();
    size_t currCount = 0;          // 当前已获得段数

    // 从所有分割字符串中查找最小的索引
    size_t index = string::npos;
    size_t splitLen = 0;
    size_t currIndex;
    for (vector<string>::const_iterator it = splitStr.begin(); it != splitStr.end(); ++it)
    {
        if (it->length() == 0)
        {
            continue;
        }

        currIndex = str.find(*it);
        if (currIndex != string::npos && currIndex < index)
        {
            index = currIndex;
            splitLen = it->length();
        }
    }

    while (index != string::npos)
    {
        if (index != 0 || !removeEmptyElm)
        {
            // 将找到的字符串放入结果中
            ++currCount;
            if (maxCount > 0 && currCount >= maxCount)
            {
                break;
            }
            result.push_back(str.substr(0, index));
        }

        // 将之前的字符和分割符都删除
        str.erase(str.begin(), str.begin() + index + splitLen);

        // 继续查找下一个
        index = string::npos;
        for (vector<string>::const_iterator it = splitStr.begin(); it != splitStr.end(); ++it)
        {
            if (it->length() == 0)
            {
                continue;
            }

            currIndex = str.find(*it);
            if (currIndex != string::npos && currIndex < index)
            {
                index = currIndex;
                splitLen = it->length();
            }
        }
    }

    // 把剩下的放进去
    if (str.length() > 0 || !removeEmptyElm)
    {
        result.push_back(str);
    }
}

void SplitStr(string str, const string &splitStr, vector<string> &result, bool removeEmptyElm = true, size_t maxCount = 0)
{
    SplitStr(str, vector<string>(1, splitStr), result, removeEmptyElm, maxCount);
}

// TODO(moon)
// 基础功能
//  nowatch事件，如何区分是哪个？或者用另一种方式确定？
//  内部暂时没做授权失败的Watcher通知处理
//  使用zoo_set_log_stream设置日志的输出文件
// 高级功能
//  分布式锁（通过最小临时节点实现）
//  Leader选举（通过最小临时节点实现）
// 额外功能
//  协程？

// 路径对应全局Watcher的类型，多个类型可以共存，彼此相或
enum GlobalWatcherType
{
    WATCHER_GET = 1,
    WATCHER_EXISTS = 3,         // 掩码包含GET
    WATCHER_GET_CHILDREN = 4
};

class ZookeeperCtx
{
public:
    enum WatcherType
    {
        NOT_WATCHER,
        GLOBAL,
        EXISTS,
        GET,
        GET_CHILDREN,
    };

    ZookeeperCtx(ZookeeperManager &zookeeper_manager, WatcherType watcher_type = NOT_WATCHER,
                 bool need_reg_watcher = true)
        :m_zookeeper_manager(zookeeper_manager), m_is_stop(false),
        m_auto_reg_watcher(need_reg_watcher), m_watcher_type(watcher_type),
        m_global_watcher_add_type(0)
    {
    }

    virtual ~ZookeeperCtx()
    {
    }

    ZookeeperManager &m_zookeeper_manager;

    shared_ptr<WatcherFunType> m_watcher_fun;
    shared_ptr<VoidCompletionFunType> m_void_completion_fun;
    shared_ptr<StatCompletionFunType> m_stat_completion_fun;
    shared_ptr<DataCompletionFunType> m_data_completion_fun;
    shared_ptr<StringsStatCompletionFunType> m_strings_stat_completion_fun;
    shared_ptr<StringCompletionFunType> m_string_completion_fun;
    shared_ptr<AclCompletionFunType> m_acl_completion_fun;
    shared_ptr<MultiCompletionFunType> m_multi_completion_fun;

    // 是否停止，停止后会释放此context，并且不会通知用户
    bool m_is_stop;

    // 是否自动重新注册Watcher，重新注册的话，会使用原有的Context
    bool m_auto_reg_watcher;

    // 当前ctx是由什么操作触发的
    WatcherType m_watcher_type;

    string m_ephemeral_path;                                // 临时节点信息，不为空时用于异步操作成功后写入临时节点信息，如果m_ephemeral_path不为空，m_ephemeral_info为NULL，表示临时节点信息被删除，在VoidCompletionFunType中需要处理
    shared_ptr<EphemeralNodeInfo> m_ephemeral_info;         // 临时节点信息，不为NULL时用于异步操作成功后写入临时节点信息

    string m_watch_path;                                    // Watch的Path，用于异步操作成功后添加Watcher信息
    shared_ptr<ZookeeperCtx> m_custom_watcher_context;      // 自定义Watcher的Context，用于异步操作成功后添加Watcher信息
    uint8_t m_global_watcher_add_type;                      // 全局Watcher要添加的类型，用于异步操作成功后添加全局Watcher信息

    // 批量操作相关数据
    shared_ptr<MultiOps> m_multi_ops;                       // 批量操作请求
    shared_ptr<vector<zoo_op_result_t>> m_multi_results;    // 批量操作结果

private:

    ZookeeperCtx(const ZookeeperCtx &right) = delete;
    ZookeeperCtx(const ZookeeperCtx &&right) = delete;
    ZookeeperCtx &operator=(const ZookeeperCtx &right) = delete;
};

ZookeeperManager::ZookeeperManager() : m_dont_close(false), m_zhandle(NULL), m_zk_tid(0), m_need_resume_env(false)
{
    m_zk_client_id.client_id = 0;
}

#ifdef CPP_ZK_USE_BOOST
int32_t ZookeeperManager::InitFromFile(const string &config_file_path, const clientid_t *client_id/*= NULL*/)
{
    try
    {
        boost::property_tree::ptree zk_conf_pt;
        read_xml(config_file_path, zk_conf_pt);

        string hosts = zk_conf_pt.get<string>("ZkConf.Hosts", "");
        string root_path = zk_conf_pt.get<string>("ZkConf.Root", "/");

        return Init(hosts, root_path, client_id);
    }
    catch (const boost::property_tree::xml_parser_error &e)
    {
        ERR_LOG(0, 0, "从配置文件[%s]中读取zookeeper路径失败[%s].", config_file_path.c_str(), e.what());
        return ZSYSTEMERROR;
    }
}
#endif

int32_t ZookeeperManager::Init(const string &hosts, const string &root_path /*= "/"*/,
                               const clientid_t *client_id/*= NULL*/)
{
    m_hosts = hosts;
    m_root_path = root_path;
    m_need_resume_env = false;

    if (m_root_path.empty() || m_root_path[0] != '/')
    {
        ERR_LOG(0, 0, "无效的API根路径[%s].", m_root_path.c_str());
        return ZBADARGUMENTS;
    }

    zoo_set_debug_level(ZOO_LOG_LEVEL_ERROR);
    if (client_id != NULL)
    {
        m_zk_client_id = *client_id;
    }

    DEBUG_LOG(0, 0, "Zookeeper配置:m_hosts[%s],m_root_path[%s],client_id[%ld].",
              m_hosts.c_str(), m_root_path.c_str(), m_zk_client_id.client_id);
    return ZOK;
}

int32_t ZookeeperManager::Connect(shared_ptr<WatcherFunType> global_watcher_fun,
                                  int32_t recv_timeout_ms, uint32_t conn_timeout_ms /*= 0*/)
{
    m_zk_tid = 0;
    if (m_zhandle != NULL)
    {
        zookeeper_close(m_zhandle);
        m_zhandle = NULL;
    }

    m_need_resume_env = true;

    // Watcher已经变了，重置
    if (m_global_watcher_context == NULL || m_global_watcher_context->m_watcher_fun != global_watcher_fun)
    {
        m_global_watcher_context = make_shared<ZookeeperCtx>(*this, ZookeeperCtx::GLOBAL);
        m_global_watcher_context->m_watcher_fun = global_watcher_fun;
    }

    m_zhandle = zookeeper_init(m_hosts.c_str(), &ZookeeperManager::InnerWatcher, recv_timeout_ms,
                               m_zk_client_id.client_id != 0 ? &m_zk_client_id : NULL,
                               m_global_watcher_context.get(), 0);
    if (m_zhandle == NULL)
    {
        ERR_LOG(0, 0, "Zookeeper:zookeeper_init错误,返回句柄为NULL,host[%s],errno[%d],error[%s].",
                m_hosts.c_str(), errno, zerror(errno));
        if (errno == ZOK)
        {
            return ZSYSTEMERROR;
        }

        return errno;
    }

    // 等待连接建立
    if (syscall(__NR_gettid) != m_zk_tid)
    {
        INFO_LOG(0, 0, "Zookeeper:开始连接.");
        unique_lock<mutex> conn_lock(m_connect_lock);
        while (GetStatus() != ZOO_CONNECTED_STATE)
        {
            if (conn_timeout_ms > 0)
            {
                m_connect_cond.wait_for(conn_lock, chrono::milliseconds(conn_timeout_ms));
                if (GetStatus() != ZOO_CONNECTED_STATE)
                {
                    ERR_LOG(0, 0, "Zookeeper:连接超时.");
                    return ZOPERATIONTIMEOUT;
                }
            }
            else
            {
                m_connect_cond.wait(conn_lock);
            }
        }

        const clientid_t *p_curr_client_id = zoo_client_id(m_zhandle);
        if (p_curr_client_id != NULL)
        {
            m_zk_client_id = *p_curr_client_id;
        }

        INFO_LOG(0, 0, "Zookeeper:已连接,client_id[%ld].", m_zk_client_id.client_id);
    }

    return ZOK;
}

int32_t ZookeeperManager::Reconnect()
{
    INFO_LOG(0, 0, "Zookeeper:开始重连.");

    // 清空ClientID，因为session过期才会进行重连，此时ClinetID已经无效了
    m_zk_client_id.client_id = 0;
    ZookeeperCtx *p_context = const_cast<ZookeeperCtx *>(reinterpret_cast<const ZookeeperCtx *>(zoo_get_context(m_zhandle)));
    if (p_context == NULL)
    {
        ERR_LOG(0, 0, "Zookeeper:无上下文.");
        return ZSYSTEMERROR;
    }

    return Connect(p_context->m_watcher_fun, zoo_recv_timeout(m_zhandle));
}

ZookeeperManager::~ZookeeperManager()
{
    if (m_zhandle != NULL && !m_dont_close)
    {
        zookeeper_close(m_zhandle);
        m_zhandle = NULL;
    }
}

int32_t ZookeeperManager::AExists(const string &path, shared_ptr<StatCompletionFunType> stat_completion_fun,
                                  int watch /*= 0*/)
{
    int32_t ret = ZOK;
    ZookeeperCtx *p_zookeeper_context = new ZookeeperCtx(*this);
    p_zookeeper_context->m_stat_completion_fun = stat_completion_fun;

    string abs_path = move(ChangeToAbsPath(path));
    if (watch != 0)
    {
        p_zookeeper_context->m_watch_path = abs_path;
        p_zookeeper_context->m_global_watcher_add_type = WATCHER_EXISTS;
    }

    ret = zoo_aexists(m_zhandle, abs_path.c_str(), watch, &ZookeeperManager::InnerStatCompletion,
                      p_zookeeper_context);
    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
        delete p_zookeeper_context;
    }

    return ret;
}

int32_t ZookeeperManager::AExists(const string &path, shared_ptr<StatCompletionFunType> stat_completion_fun,
                                  shared_ptr<WatcherFunType> watcher_fun)
{
    int32_t ret = ZOK;
    ZookeeperCtx *p_zookeeper_context = new ZookeeperCtx(*this);
    p_zookeeper_context->m_stat_completion_fun = stat_completion_fun;

    shared_ptr<ZookeeperCtx> p_zookeeper_watcher_context = make_shared<ZookeeperCtx>(*this, ZookeeperCtx::EXISTS);
    p_zookeeper_watcher_context->m_watcher_fun = watcher_fun;

    string abs_path = move(ChangeToAbsPath(path));
    p_zookeeper_context->m_watch_path = abs_path;
    p_zookeeper_context->m_custom_watcher_context = p_zookeeper_watcher_context;

    ret = zoo_awexists(m_zhandle, abs_path.c_str(), &ZookeeperManager::InnerWatcher, p_zookeeper_watcher_context.get(),
                       &ZookeeperManager::InnerStatCompletion, p_zookeeper_context);
    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
        delete p_zookeeper_context;
    }

    return ret;
}

int32_t ZookeeperManager::Exists(const string &path, Stat *stat, int watch /*= 0*/)
{
    string abs_path = move(ChangeToAbsPath(path));
    int32_t ret = zoo_exists(m_zhandle, abs_path.c_str(), watch, stat);
    if (ret == ZOK || ret == ZNONODE)
    {
        if (watch != 0)
        {
            unique_lock<recursive_mutex> lock(m_global_watcher_path_type_lock);
            m_global_watcher_path_type[abs_path] |= WATCHER_EXISTS;
        }
    }
    else
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
    }

    return ret;
}

int32_t ZookeeperManager::Exists(const string &path, Stat *stat, shared_ptr<WatcherFunType> watcher_fun)
{
    int32_t ret = ZOK;
    shared_ptr<ZookeeperCtx> p_zookeeper_watcher_context = make_shared<ZookeeperCtx>(*this, ZookeeperCtx::EXISTS);
    p_zookeeper_watcher_context->m_watcher_fun = watcher_fun;

    string abs_path = move(ChangeToAbsPath(path));
    ret = zoo_wexists(m_zhandle, abs_path.c_str(), &ZookeeperManager::InnerWatcher, p_zookeeper_watcher_context.get(), stat);
    if (ret == ZOK || ret == ZNONODE)
    {
        AddCustomWatcher(abs_path, p_zookeeper_watcher_context);
    }
    else
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
    }

    return ret;
}

int32_t ZookeeperManager::AGet(const string &path,
                               shared_ptr<DataCompletionFunType> data_completion_fun, int watch /*= 0*/)
{
    int32_t ret = ZOK;
    ZookeeperCtx *p_zookeeper_context = new ZookeeperCtx(*this);
    p_zookeeper_context->m_data_completion_fun = data_completion_fun;

    string abs_path = move(ChangeToAbsPath(path));
    if (watch != 0)
    {
        p_zookeeper_context->m_watch_path = abs_path;
        p_zookeeper_context->m_global_watcher_add_type = WATCHER_GET;
    }

    ret = zoo_aget(m_zhandle, abs_path.c_str(), watch, &ZookeeperManager::InnerDataCompletion, p_zookeeper_context);
    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
        delete p_zookeeper_context;
    }

    return ret;
}

int32_t ZookeeperManager::AGet(const string &path, shared_ptr<DataCompletionFunType> data_completion_fun,
                               shared_ptr<WatcherFunType> watcher_fun)
{
    int32_t ret = ZOK;
    ZookeeperCtx *p_zookeeper_context = new ZookeeperCtx(*this);
    p_zookeeper_context->m_data_completion_fun = data_completion_fun;

    shared_ptr<ZookeeperCtx> p_zookeeper_watcher_context = make_shared<ZookeeperCtx>(*this, ZookeeperCtx::GET);
    p_zookeeper_watcher_context->m_watcher_fun = watcher_fun;

    string abs_path = move(ChangeToAbsPath(path));
    p_zookeeper_context->m_watch_path = abs_path;
    p_zookeeper_context->m_custom_watcher_context = p_zookeeper_watcher_context;

    ret = zoo_awget(m_zhandle, abs_path.c_str(), &ZookeeperManager::InnerWatcher, p_zookeeper_watcher_context.get(),
                    &ZookeeperManager::InnerDataCompletion, p_zookeeper_context);
    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
        delete p_zookeeper_context;
    }

    return ret;
}

int32_t ZookeeperManager::Get(const string &path, char *buffer, int* buflen, Stat *stat, int watch /*= 0*/)
{
    string abs_path = move(ChangeToAbsPath(path));
    int32_t ret = zoo_get(m_zhandle, abs_path.c_str(), watch, buffer, buflen, stat);
    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
    }
    else if (watch != 0)
    {
        unique_lock<recursive_mutex> lock(m_global_watcher_path_type_lock);
        m_global_watcher_path_type[abs_path] |= WATCHER_GET;
    }
    else
    {
        // Noting
    }

    return ret;
}

int32_t ZookeeperManager::Get(const string &path, char *buffer, int* buflen, Stat *stat,
                              shared_ptr<WatcherFunType> watcher_fun)
{
    int32_t ret = ZOK;
    shared_ptr<ZookeeperCtx> p_zookeeper_watcher_context = make_shared<ZookeeperCtx>(*this, ZookeeperCtx::GET);
    p_zookeeper_watcher_context->m_watcher_fun = watcher_fun;

    string abs_path = move(ChangeToAbsPath(path));
    ret = zoo_wget(m_zhandle, abs_path.c_str(), &ZookeeperManager::InnerWatcher,
                   p_zookeeper_watcher_context.get(), buffer, buflen, stat);
    if (ret == ZOK)
    {
        AddCustomWatcher(abs_path, p_zookeeper_watcher_context);
    }
    else
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
    }

    return ret;
}

int32_t ZookeeperManager::AGetChildren(const string &path,
                                       shared_ptr<StringsStatCompletionFunType> strings_stat_completion_fun,
                                       int watch /*= 0*/, bool need_stat /*= false*/)
{
    int32_t ret = ZOK;
    ZookeeperCtx *p_zookeeper_context = new ZookeeperCtx(*this);
    p_zookeeper_context->m_strings_stat_completion_fun = strings_stat_completion_fun;
    string abs_path = move(ChangeToAbsPath(path));
    if (watch != 0)
    {
        p_zookeeper_context->m_watch_path = abs_path;
        p_zookeeper_context->m_global_watcher_add_type = WATCHER_GET_CHILDREN;
    }

    if (need_stat)
    {
        ret = zoo_aget_children2(m_zhandle, abs_path.c_str(), watch,
                                 &ZookeeperManager::InnerStringsStatCompletion, p_zookeeper_context);

    }
    else
    {
        ret = zoo_aget_children(m_zhandle, abs_path.c_str(), watch,
                                &ZookeeperManager::InnerStringsCompletion, p_zookeeper_context);
    }

    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
        delete p_zookeeper_context;
    }

    return ret;
}

int32_t ZookeeperManager::AGetChildren(const string &path,
                                       shared_ptr<StringsStatCompletionFunType> strings_stat_completion_fun,
                                       shared_ptr<WatcherFunType> watcher_fun, bool need_stat /*= false*/)
{
    // 此处需要创建2个context
    int32_t ret = ZOK;

    ZookeeperCtx *p_zookeeper_context = new ZookeeperCtx(*this);
    p_zookeeper_context->m_strings_stat_completion_fun = strings_stat_completion_fun;

    shared_ptr<ZookeeperCtx> p_zookeeper_watcher_context = make_shared<ZookeeperCtx>(*this, ZookeeperCtx::GET_CHILDREN);
    p_zookeeper_watcher_context->m_watcher_fun = watcher_fun;

    string abs_path = move(ChangeToAbsPath(path));
    p_zookeeper_context->m_watch_path = abs_path;
    p_zookeeper_context->m_custom_watcher_context = p_zookeeper_watcher_context;

    if (need_stat)
    {
        ret = zoo_awget_children2(m_zhandle, abs_path.c_str(), &ZookeeperManager::InnerWatcher,
                                  p_zookeeper_watcher_context.get(),
                                  &ZookeeperManager::InnerStringsStatCompletion, p_zookeeper_context);
    }
    else
    {
        ret = zoo_awget_children(m_zhandle, abs_path.c_str(), &ZookeeperManager::InnerWatcher,
                                 p_zookeeper_watcher_context.get(),
                                 &ZookeeperManager::InnerStringsCompletion, p_zookeeper_context);
    }

    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
        delete p_zookeeper_context;
    }

    return ret;
}

int32_t ZookeeperManager::GetChildren(const string &path, ScopedStringVector &strings,
                                      int watch /*= 0*/, Stat *stat /*= NULL*/)
{
    // 这里要Clear掉它，避免内部还有数据时导致内存泄露
    strings.Clear();
    string abs_path = move(ChangeToAbsPath(path));
    int32_t ret = ZOK;
    if (stat == NULL)
    {
        ret = zoo_get_children(m_zhandle, abs_path.c_str(), watch, &strings);
    }
    else
    {
        ret = zoo_get_children2(m_zhandle, abs_path.c_str(), watch, &strings, stat);
    }

    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
    }
    else if (watch != 0)
    {
        unique_lock<recursive_mutex> lock(m_global_watcher_path_type_lock);
        m_global_watcher_path_type[abs_path] |= WATCHER_GET_CHILDREN;
    }
    else
    {
        // Nothing
    }

    return ret;
}

int32_t ZookeeperManager::GetChildren(const string &path, ScopedStringVector &strings,
                                      shared_ptr<WatcherFunType> watcher_fun, Stat *stat /*= NULL*/)
{
    strings.Clear();
    int32_t ret = ZOK;
    shared_ptr<ZookeeperCtx> p_zookeeper_watcher_context = make_shared<ZookeeperCtx>(*this, ZookeeperCtx::GET_CHILDREN);
    p_zookeeper_watcher_context->m_watcher_fun = watcher_fun;

    string abs_path = move(ChangeToAbsPath(path));
    if (stat == NULL)
    {
        ret = zoo_wget_children(m_zhandle, abs_path.c_str(),
                                &ZookeeperManager::InnerWatcher, p_zookeeper_watcher_context.get(), &strings);
    }
    else
    {
        ret = zoo_wget_children2(m_zhandle, abs_path.c_str(),
                                 &ZookeeperManager::InnerWatcher, p_zookeeper_watcher_context.get(),
                                 &strings, stat);
    }

    if (ret == ZOK)
    {
        AddCustomWatcher(abs_path, p_zookeeper_watcher_context);
    }
    else
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
    }

    return ret;
}

int32_t ZookeeperManager::ACreate(const string &path, const char *value, int valuelen,
                                  shared_ptr<StringCompletionFunType> string_completion_fun,
                                  const ACL_vector *acl /*= &ZOO_OPEN_ACL_UNSAFE*/, int flags /*= 0*/)
{
    int32_t ret = ZOK;
    ZookeeperCtx *p_zookeeper_context = new ZookeeperCtx(*this);
    string abs_path = move(ChangeToAbsPath(path));

    p_zookeeper_context->m_string_completion_fun = string_completion_fun;
    if (flags & ZOO_EPHEMERAL)
    {
        p_zookeeper_context->m_ephemeral_path = abs_path;
        p_zookeeper_context->m_ephemeral_info = make_shared<EphemeralNodeInfo>();
        p_zookeeper_context->m_ephemeral_info->Acl = *acl;
        p_zookeeper_context->m_ephemeral_info->Data.assign(value, valuelen);
        p_zookeeper_context->m_ephemeral_info->Flags = flags;
    }

    ret = zoo_acreate(m_zhandle, abs_path.c_str(), value, valuelen, acl, flags,
                      &ZookeeperManager::InnerStringCompletion, p_zookeeper_context);

    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
        delete p_zookeeper_context;
    }

    return ret;
}

int32_t ZookeeperManager::ACreate(const string &path, const string &value,
                                  shared_ptr<StringCompletionFunType> string_completion_fun,
                                  const ACL_vector *acl /*= &ZOO_OPEN_ACL_UNSAFE*/, int flags /*= 0*/)
{
    return ACreate(path, value.data(), value.size(), string_completion_fun, acl, flags);
}

int32_t ZookeeperManager::Create(const string &path, const char *value, int valuelen,
                                 string *p_real_path /*= NULL*/, const ACL_vector *acl /*= &ZOO_OPEN_ACL_UNSAFE*/,
                                 int flags /*= 0*/, bool ephemeral_exist_skip /*= false*/)
{
    string abs_path = move(ChangeToAbsPath(path));
    int32_t ret;
    string exist_value;             // 节点存在的话，保存其Value
    string exist_path;              // 节点存在的话，保存其路径，不为空，表示节点存在
    Stat exist_stat;                // 节点存在的话，保存其stat
    bzero(&exist_stat, sizeof(exist_stat));

    // 重连恢复API临时节点状态步骤
    if (ephemeral_exist_skip && (flags & ZOO_EPHEMERAL))
    {
        exist_value.resize(valuelen);
        if (flags & ZOO_SEQUENCE)
        {
            // 如果是序列节点，获得当前父节点所有的子节点，判断有没有正则表达式为"[节点名]\w{10}"的节点
            // 有的话，获得他们的owner信息，如果找到了，则将此节点加入到m_ephemeral_node_info中
            // 这个只能适用于一个同名节点的情况，如果有超过1个以上的同名节点，则不支持，目前也没有这样的需求，比如创建2个名为node，flag为ZOO_EPHEMERAL|ZOO_SEQUENCE的节点

            // 获得节点名和父路径
            size_t index = abs_path.rfind('/');
            if (index == string::npos)
            {
                ERR_LOG(0, 0, "无法获得路径[%s]的父路径.", abs_path.c_str());
                return ZBADARGUMENTS;
            }

            string parent_path = abs_path.substr(0, index);
            string node_name = abs_path.substr(index + 1);

            // 获得所有子节点
            ScopedStringVector children;
            ret = GetChildren(parent_path, children);
            if (ret != ZOK)
            {
                ERR_LOG(0, 0, "Zookeeper:发生错误:parent_path[%s],ret[%d],zerror[%s].", parent_path.c_str(), ret, zerror(ret));
                return ret;
            }

            static const uint32_t SEQUENCE_LEN = 10;            // 序号长度，全是数字
            list<string> match_children;                        // 符合条件的children
            for (int32_t ci = 0; ci < children.count; ++ci)
            {
                char *child = children.GetData(ci);
                uint32_t children_len = strlen(child);

                // 序号节点名长度 = 原节点名长度 + SEQUENCE_LEN，不符合的跳过
                if (node_name.size() + SEQUENCE_LEN != children_len)
                {
                    continue;
                }

                // 如果节点名前node_name.size()不一样，跳过
                if (memcmp(child, node_name.c_str(), node_name.size()) != 0)
                {
                    continue;
                }

                // 判断children后SEQUENCE_LEN个字符是不是都是数字，如果不是，跳过
                uint32_t i = node_name.size();
                for (; i < node_name.size() + SEQUENCE_LEN; ++i)
                {
                    if (!isdigit(child[i]))
                    {
                        break;
                    }
                }

                if (i == node_name.size() + SEQUENCE_LEN)
                {
                    // 符合条件
                    match_children.push_back(child);
                }
            }

            // 获得所有符合条件的节点Stat，判断owner是否是自己
            for (auto child_it = match_children.begin(); child_it != match_children.end(); ++child_it)
            {
                int buflen = valuelen;
                string child_path = parent_path + "/" + *child_it;
                ret = Get(child_path, const_cast<char *>(exist_value.data()), &buflen, &exist_stat);
                if (ret != ZOK)
                {
                    ERR_LOG(0, 0, "Zookeeper:发生错误:child_path[%s],ret[%d],zerror[%s].", child_path.c_str(), ret, zerror(ret));
                    return ret;
                }

                if (exist_stat.ephemeralOwner == m_zk_client_id.client_id)
                {
                    // 找到了，返回
                    exist_path = child_path;
                    break;
                }
            }
        }
        else
        {
            // 如果是普通临时节点，直接Get出Stat判断Owner即可
            char child_buf[1];
            int buflen = sizeof(child_buf);
            ret = Get(path, child_buf, &buflen, &exist_stat);
            if (ret != ZOK)
            {
                ERR_LOG(0, 0, "Zookeeper:发生错误:child_path[%s],ret[%d],zerror[%s].", path.c_str(), ret, zerror(ret));
                return ret;
            }

            if (exist_stat.ephemeralOwner == m_zk_client_id.client_id)
            {
                exist_path = path;
            }
        }
    }

    if (!exist_path.empty())
    {
        if (p_real_path != NULL)
        {
            strncpy(const_cast<char *>(p_real_path->data()), exist_path.c_str(), p_real_path->size());
        }

        // 已经存在，处理value，如果Value不同，则重新写入
        if (memcmp(exist_value.c_str(), value, valuelen) != 0)
        {
            ret = Set(exist_path, value, valuelen, exist_stat.version);
            if (ret != ZOK)
            {
                ERR_LOG(0, 0, "Zookeeper:发生错误:exist_path[%s],ret[%d],zerror[%s].", exist_path.c_str(), ret, zerror(ret));
                return ret;
            }
        }

        // TODO(moon)：这里没有判断ACL，目前没需求，后面有的话，要加上判断
    }
    else
    {
        ret = zoo_create(m_zhandle, abs_path.c_str(), value, valuelen, acl, flags,
                         p_real_path != NULL ? const_cast<char *>(p_real_path->data()) : NULL,
                         p_real_path != NULL ? p_real_path->size() : 0);

        if (ret != ZOK)
        {
            ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
            return ret;
        }
    }

    // 调用成功
    if (flags & ZOO_EPHEMERAL)
    {
        // 如果是临时节点，添加到临时节点列表中。
        unique_lock<recursive_mutex> phemeral_node_info_lock(m_ephemeral_node_info_lock);
        m_ephemeral_node_info[abs_path].Acl = *acl;
        m_ephemeral_node_info[abs_path].Data.assign(value, valuelen);
        m_ephemeral_node_info[abs_path].Flags = flags;
    }

    return ZOK;
}

int32_t ZookeeperManager::Create(const string &path, const string &value, string *p_real_path /*= NULL*/,
                                 const ACL_vector *acl /*= &ZOO_OPEN_ACL_UNSAFE*/, int flags /*= 0*/,
                                 bool ephemeral_exist_skip /*= false*/)
{
    return Create(path, value.data(), value.size(), p_real_path, acl, flags, ephemeral_exist_skip);
}

int32_t ZookeeperManager::ASet(const string &path, const char *buffer, int buflen,
                               int version, shared_ptr<StatCompletionFunType> stat_completion_fun)
{
    int32_t ret = ZOK;
    ZookeeperCtx *p_zookeeper_context = new ZookeeperCtx(*this);
    p_zookeeper_context->m_stat_completion_fun = stat_completion_fun;
    string abs_path = move(ChangeToAbsPath(path));

    unique_lock<recursive_mutex> phemeral_node_info_lock(m_ephemeral_node_info_lock);
    auto ephemeral_it = m_ephemeral_node_info.find(abs_path);
    if (ephemeral_it != m_ephemeral_node_info.end())
    {
        p_zookeeper_context->m_ephemeral_path = abs_path;
        p_zookeeper_context->m_ephemeral_info = make_shared<EphemeralNodeInfo>();
        *p_zookeeper_context->m_ephemeral_info = ephemeral_it->second;
        p_zookeeper_context->m_ephemeral_info->Data.assign(buffer, buflen);
    }
    phemeral_node_info_lock.unlock();

    ret = zoo_aset(m_zhandle, abs_path.c_str(), buffer, buflen, version, &ZookeeperManager::InnerStatCompletion,
                   p_zookeeper_context);
    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
        delete p_zookeeper_context;
    }

    return ret;
}

int32_t ZookeeperManager::ASet(const string &path, const string &buffer, int version,
                               shared_ptr<StatCompletionFunType> stat_completion_fun)
{
    return ASet(path, buffer.data(), buffer.size(), version, stat_completion_fun);
}

int32_t ZookeeperManager::Set(const string &path, const char *buffer, int buflen, int version, Stat *stat /*= NULL*/)
{
    int32_t ret = ZOK;
    string abs_path = move(ChangeToAbsPath(path));
    if (stat == NULL)
    {
        ret = zoo_set(m_zhandle, abs_path.c_str(), buffer, buflen, version);
    }
    else
    {
        ret = zoo_set2(m_zhandle, abs_path.c_str(), buffer, buflen, version, stat);
    }

    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
    }
    else
    {
        // 调用成功
        unique_lock<recursive_mutex> phemeral_node_info_lock(m_ephemeral_node_info_lock);
        auto ephemeral_it = m_ephemeral_node_info.find(abs_path);
        if (ephemeral_it != m_ephemeral_node_info.end())
        {
            // 如果在临时节点列表中找到，修改数据
            ephemeral_it->second.Data.assign(buffer, buflen);
        }
    }

    return ret;
}

int32_t ZookeeperManager::Set(const string &path, const string &buffer, int version, Stat *stat /*= NULL*/)
{
    return Set(path, buffer.data(), buffer.size(), version, stat);
}

int32_t ZookeeperManager::ADelete(const string &path, int version,
                                  shared_ptr<VoidCompletionFunType> void_completion_fun)
{
    int32_t ret = ZOK;
    ZookeeperCtx *p_zookeeper_context = new ZookeeperCtx(*this);
    p_zookeeper_context->m_void_completion_fun = void_completion_fun;
    string abs_path = move(ChangeToAbsPath(path));

    unique_lock<recursive_mutex> phemeral_node_info_lock(m_ephemeral_node_info_lock);
    auto ephemeral_it = m_ephemeral_node_info.find(abs_path);
    if (ephemeral_it != m_ephemeral_node_info.end())
    {
        p_zookeeper_context->m_ephemeral_path = abs_path;
    }
    phemeral_node_info_lock.unlock();

    ret = zoo_adelete(m_zhandle, abs_path.c_str(), version, &ZookeeperManager::InnerVoidCompletion, p_zookeeper_context);

    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
        delete p_zookeeper_context;
    }

    return ret;
}

int32_t ZookeeperManager::Delete(const string &path, int version)
{
    string abs_path = move(ChangeToAbsPath(path));
    int32_t ret = zoo_delete(m_zhandle, abs_path.c_str(), version);
    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
    }
    else
    {
        // 调用成功
        unique_lock<recursive_mutex> phemeral_node_info_lock(m_ephemeral_node_info_lock);
        if (m_ephemeral_node_info.find(abs_path) != m_ephemeral_node_info.end())
        {
            // 如果在临时节点列表中找到，删除它
            m_ephemeral_node_info.erase(abs_path);
        }
    }

    return ret;
}

int32_t ZookeeperManager::AGetAcl(const string &path, shared_ptr<AclCompletionFunType> acl_completion_fun)
{
    int32_t ret = ZOK;
    ZookeeperCtx *p_zookeeper_context = new ZookeeperCtx(*this);
    p_zookeeper_context->m_acl_completion_fun = acl_completion_fun;
    string abs_path = move(ChangeToAbsPath(path));
    ret = zoo_aget_acl(m_zhandle, abs_path.c_str(), &ZookeeperManager::InnerAclCompletion, p_zookeeper_context);

    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
        delete p_zookeeper_context;
    }

    return ret;
}

int32_t ZookeeperManager::GetAcl(const string &path, ScopedAclVector &acl, Stat *stat)
{
    acl.Clear();
    string abs_path = move(ChangeToAbsPath(path));
    int32_t ret = zoo_get_acl(m_zhandle, abs_path.c_str(), &acl, stat);
    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
    }

    return ret;
}

int32_t ZookeeperManager::ASetAcl(const string &path, int version, ACL_vector *acl,
                                  shared_ptr<VoidCompletionFunType> void_completion_fun)
{
    int32_t ret = ZOK;
    ZookeeperCtx *p_zookeeper_context = new ZookeeperCtx(*this);
    p_zookeeper_context->m_void_completion_fun = void_completion_fun;
    string abs_path = move(ChangeToAbsPath(path));

    unique_lock<recursive_mutex> phemeral_node_info_lock(m_ephemeral_node_info_lock);
    auto ephemeral_it = m_ephemeral_node_info.find(abs_path);
    if (ephemeral_it != m_ephemeral_node_info.end())
    {
        p_zookeeper_context->m_ephemeral_path = abs_path;
        p_zookeeper_context->m_ephemeral_info = make_shared<EphemeralNodeInfo>();
        *p_zookeeper_context->m_ephemeral_info = ephemeral_it->second;
        p_zookeeper_context->m_ephemeral_info->Acl = *acl;
    }
    phemeral_node_info_lock.unlock();

    ret = zoo_aset_acl(m_zhandle, abs_path.c_str(), version, acl, &ZookeeperManager::InnerVoidCompletion, p_zookeeper_context);

    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
        delete p_zookeeper_context;
    }

    return ret;
}

int32_t ZookeeperManager::SetAcl(const string &path, int version, ACL_vector *acl)
{
    string abs_path = move(ChangeToAbsPath(path));
    int32_t ret = zoo_set_acl(m_zhandle, abs_path.c_str(), version, acl);
    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:abs_path[%s],ret[%d],zerror[%s].", abs_path.c_str(), ret, zerror(ret));
    }
    else
    {
        // 调用成功
        unique_lock<recursive_mutex> phemeral_node_info_lock(m_ephemeral_node_info_lock);
        auto ephemeral_it = m_ephemeral_node_info.find(abs_path);
        if (ephemeral_it != m_ephemeral_node_info.end())
        {
            // 如果在临时节点列表中找到，修改数据
            ephemeral_it->second.Acl = *acl;
        }
    }

    return ret;
}

MultiOps ZookeeperManager::CreateMultiOps()
{
    return MultiOps(this);
}

int32_t ZookeeperManager::AMulti(shared_ptr<MultiOps> &multi_ops,
                                 shared_ptr<MultiCompletionFunType> multi_completion_fun)
{
    if (multi_ops->m_multi_ops.empty())
    {
        WARN_LOG(0, 0, "批量操作数量为0.");
        return ZBADARGUMENTS;
    }

    int32_t ret = ZOK;
    ZookeeperCtx *p_zookeeper_context = new ZookeeperCtx(*this);
    p_zookeeper_context->m_multi_completion_fun = multi_completion_fun;
    p_zookeeper_context->m_multi_ops = multi_ops;
    p_zookeeper_context->m_multi_results.reset(new vector<zoo_op_result_t>());
    p_zookeeper_context->m_multi_results->resize(multi_ops->m_multi_ops.size());
    ret = zoo_amulti(m_zhandle, multi_ops->m_multi_ops.size(), &multi_ops->m_multi_ops[0],
                     &(*p_zookeeper_context->m_multi_results)[0],
                     &ZookeeperManager::InnerMultiCompletion, p_zookeeper_context);

    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:批量操作数量[%lu],ret[%d],zerror[%s].",
                multi_ops->m_multi_ops.size(), ret, zerror(ret));
        delete p_zookeeper_context;
    }

    return ret;
}

int32_t ZookeeperManager::Multi(MultiOps &multi_ops, vector<zoo_op_result_t> &results)
{
    // 为了保证没有之前使用的脏数据，这里必须clear掉
    results.clear();
    results.resize(multi_ops.m_multi_ops.size());

    // TODO(moon)：官方API中如果操作数量为0怎么办？
    if (multi_ops.m_multi_ops.empty())
    {
        WARN_LOG(0, 0, "批量操作数量为0.");
        return ZBADARGUMENTS;
    }

    // TODO(moon)：注意包量总大小限制1M
    int32_t ret = zoo_multi(m_zhandle, multi_ops.m_multi_ops.size(), &multi_ops.m_multi_ops[0], &results[0]);
    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "Zookeeper:发生错误:批量操作数量[%lu],ret[%d],zerror[%s].",
                multi_ops.m_multi_ops.size(), ret, zerror(ret));
    }

    // 处理临时节点，这里可能部分成功，部分失败
    ProcMultiEphemeralNode(multi_ops.m_multi_ops, results);

    return ret;
}

const string ZookeeperManager::ChangeToAbsPath(const string &path)
{
    // 为空，返回根目录
    if (path.empty())
    {
        return m_root_path;
    }

    // 本来就是绝对路径，直接返回
    if (path[0] == '/')
    {
        return path;
    }

    // 相对路径的处理
    if (*m_root_path.rbegin() == '/')
    {
        // 如果是绝对根目录，根目录后不加'/'
        return m_root_path + path;
    }

    return m_root_path + "/" + path;
}

int32_t ZookeeperManager::CreatePathRecursion(const string &path)
{
    int32_t ret;

    // 创建节点，忽略节点已存在的错误
    string abs_path = move(ChangeToAbsPath(path));
    vector<string> dirs;
    SplitStr(abs_path, "/", dirs);
    string curr_path;

    // 先使用批量check接口逐步判断节点是否存在，对不存在的节点进行批量创建
    MultiOps multi_check_ops(this);
    for (auto dir_it = dirs.begin(); dir_it != dirs.end(); ++dir_it)
    {
        curr_path += string("/") + *dir_it;
        multi_check_ops.AddCheckOp(curr_path, -1);
    }

    vector<zoo_op_result_t> results;
    ret = Multi(multi_check_ops, results);
    if (ret == ZNONODE)
    {
        // 如果某一级节点不存在，则将此级及以后的节点全部批量创建
        MultiOps multi_create_ops(this);
        bool start_no_node = false;

        auto check_op_it = multi_check_ops.m_multi_ops.begin();
        for (auto result_it = results.begin();
             result_it != results.end() && check_op_it != multi_check_ops.m_multi_ops.end();
             ++result_it, ++check_op_it)
        {
            // 跳过前面已经存在的节点
            if (result_it->err == ZOK && !start_no_node)
            {
                continue;
            }

            start_no_node = true;

            multi_create_ops.AddCreateOp(check_op_it->check_op.path, "");
        }

        // 执行批量创建
        ret = Multi(multi_create_ops, results);
        if (ret != ZOK)
        {
            ERR_LOG(0, 0, "批量创建失败,ret[%d].", ret);
            return ret;
        }
    }

    return ret;
}

int32_t ZookeeperManager::DeletePathRecursion(const string &path)
{
    // 获得路径所有的子节点，按照顺序存储起来
    list<string> path_to_get_children;          // 需要获得子节点的节点，预处理节点列表
    list<string> path_to_delete;                // 需要删除的节点，越往后，节点越深，所以需要从后往前删除

    string abs_path = move(ChangeToAbsPath(path));
    path_to_get_children.push_back(abs_path);       // 将需要删除的根节点塞进去，以备获得它的子节点

    ScopedStringVector children;
    int32_t ret;
    while (!path_to_get_children.empty())
    {
        // 从预处理节点列表后面获得一个节点，采用深度遍历（栈：后进先出）
        auto curr_path = move(*path_to_get_children.rbegin());
        path_to_get_children.pop_back();
        children.Clear();
        ret = GetChildren(curr_path.c_str(), children);
        if (ret != ZOK && ret != ZNONODE)
        {
            ERR_LOG(0, 0, "递归删除节点[%s],abs_path[%s]在获得[%s]子节点时失败.", path.c_str(), abs_path.c_str(), curr_path.c_str());
            return ret;
        }

        // 节点已经不存在了，则跳过
        if (ret == ZNONODE)
        {
            continue;
        }

        for (int32_t i = 0; i < children.count; ++i)
        {
            path_to_get_children.push_back(curr_path + "/" + children.data[i]);
        }

        // 将此节点从预处理节点列表中移动到需要删除的节点列表，并且将它的所有子节点插入到预处理节点后
        path_to_delete.push_back(move(curr_path));
    }

    // 批量删除，从删除列表中从后往前添加批量删除操作
    if (!path_to_delete.empty())
    {
        MultiOps multi_delete_ops(this);
        for (auto path_it = path_to_delete.rbegin(); path_it != path_to_delete.rend(); ++path_it)
        {
            multi_delete_ops.AddDeleteOp(*path_it, -1);
        }

        vector<zoo_op_result_t> results;
        ret = Multi(multi_delete_ops, results);
        if (ret != ZOK)
        {
            ERR_LOG(0, 0, "递归删除节点[%s],abs_path[%s],失败,ret[%d].", path.c_str(), abs_path.c_str(), ret);
            return ret;
        }
    }

    // 寻找该路径下所有临时节点信息，删除，避免临时节点不在Multi操作中删除，从而漏删临时节点
    // 测试用例：ZooKeeper.ZkManagerEphemeralNodeTest
    unique_lock<recursive_mutex> phemeral_node_info_lock(m_ephemeral_node_info_lock);
    string pre_path = abs_path + "/";       // 临时节点路径前缀，注意这里是需要包含"/"的，否则会误删
    for (auto node_it = m_ephemeral_node_info.begin(); node_it != m_ephemeral_node_info.end();)
    {
        if (node_it->first.find(pre_path) == 0)
        {
            m_ephemeral_node_info.erase(node_it++);
        }
        else
        {
            ++node_it;
        }
    }

    // 再尝试删除abs_path自身
    m_ephemeral_node_info.erase(abs_path);
    phemeral_node_info_lock.unlock();

    return ZOK;
}

int32_t ZookeeperManager::GetChildrenValue(const string &path, map<string, ValueStat> &children_value,
                                           uint32_t max_value_size /*= 2048*/)
{
    ScopedStringVector children;
    string abs_path = move(ChangeToAbsPath(path));
    int32_t ret = GetChildren(abs_path, children);
    if (ret != ZOK)
    {
        ERR_LOG(0, 0, "GetChildren[%s]发生错误,ret[%d].", abs_path.c_str(), ret);
        return ret;
    }

    children_value.clear();
    for (int32_t i = 0; i < children.count; ++i)
    {
        string child_path = abs_path + "/" + children.data[i];
        auto &value_stat = children_value[children.data[i]];
        value_stat.value.resize(max_value_size);
        int value_len = max_value_size;
        ret = Get(child_path, const_cast<char *>(value_stat.value.data()),
                  &value_len, &value_stat.stat);
        if (ret != ZOK)
        {
            ERR_LOG(0, 0, "Get[%s]发生错误,ret[%d].", child_path.c_str(), ret);
            return ret;
        }

        // resize到实际长度
        value_stat.value.resize(value_len);
    }

    return ZOK;
}

int32_t ZookeeperManager::GetCString(const string &path, string &data, Stat *stat /*= NULL*/, int watch /*= 0*/)
{
    int datalen = data.size() - 1;
    int32_t ret = Get(path, const_cast<char *>(data.data()), &datalen, stat, watch);
    if (ret == ZOK && datalen < static_cast<int32_t>(data.size()))
    {
        data[datalen] = '\0';
    }

    return ret;
}

int32_t ZookeeperManager::GetCString(const string &path, string &data, Stat *stat, shared_ptr<WatcherFunType> watcher_fun)
{
    int datalen = data.size() - 1;
    int32_t ret = Get(path, const_cast<char *>(data.data()), &datalen, stat, watcher_fun);
    if (ret == ZOK && datalen < static_cast<int32_t>(data.size()))
    {
        data[datalen] = '\0';
    }

    return ret;
}

void ZookeeperManager::DeleteWatcher(int type, const char *abs_path, void *p_zookeeper_context)
{
    if (GetHandler() == NULL)
    {
        return;
    }

    // 要处理的Watcher哈希表，根据不同的type，有不同的表
    list<hashtable *> hashtables_to_search;

#define ADD_WATCHER_HASHTABLE(watchers) if ((watchers) != NULL && (watchers)->ht != NULL)hashtables_to_search.push_back((watchers)->ht)

    if (type == ZOO_CREATED_EVENT || type == ZOO_CHANGED_EVENT)
    {
        ADD_WATCHER_HASHTABLE(GetHandler()->active_node_watchers);
        ADD_WATCHER_HASHTABLE(GetHandler()->active_exist_watchers);
    }
    else if (type == ZOO_CHILD_EVENT)
    {
        ADD_WATCHER_HASHTABLE(GetHandler()->active_child_watchers);
    }
    else if (type == ZOO_DELETED_EVENT)
    {
        ADD_WATCHER_HASHTABLE(GetHandler()->active_node_watchers);
        ADD_WATCHER_HASHTABLE(GetHandler()->active_exist_watchers);
        ADD_WATCHER_HASHTABLE(GetHandler()->active_child_watchers);
    }
    else
    {
        // 无操作
    }

#undef ADD_WATCHER_HASHTABLE

    list<void *> to_free;
    for (auto hashtable_it = hashtables_to_search.begin(); hashtable_it != hashtables_to_search.end();
         ++hashtable_it)
    {
        watcher_object_list_t* wl = (watcher_object_list_t *)hashtable_search(*hashtable_it, (void*)abs_path);
        if (wl == NULL)
        {
            continue;
        }

        // 删除指定context的Watcher
        watcher_object_t *p_watcher = wl->head;
        watcher_object_t *p_last = p_watcher;
        while (p_watcher != NULL)
        {
            // 要删除
            if (p_watcher->context == p_zookeeper_context)
            {
                if (p_watcher == wl->head)
                {
                    // 头结点
                    wl->head = p_watcher->next;
                }
                else
                {
                    p_last->next = p_watcher->next;
                }

                to_free.push_back(p_watcher);
            }
            else
            {
                // 不删除
                p_last = p_watcher;
            }

            p_watcher = p_watcher->next;
        }
    }

    for (auto free_it = to_free.begin(); free_it != to_free.end(); ++free_it)
    {
        free(*free_it);
    }
}

void ZookeeperManager::InnerWatcher(zhandle_t *zh, int type, int state,
                                    const char *abs_path, void *p_zookeeper_context)
{
    if (abs_path != NULL)
    {
        DEBUG_LOG(0, 0, "Zookeeper:触发%s,type[%d],state[%d],abs_path[%p][%s],ctx[%p].",
                  __FUNCTION__, type, state, abs_path, abs_path, p_zookeeper_context);
    }
    else
    {
        INFO_LOG(0, 0, "Zookeeper:触发%s,type[%d],state[%d],abs_path[%p],ctx[%p].",
                 __FUNCTION__, type, state, abs_path, p_zookeeper_context);
    }

    ZookeeperCtx *p_context = const_cast<ZookeeperCtx *>(reinterpret_cast<const ZookeeperCtx *>(p_zookeeper_context));
    if (p_context == NULL)
    {
        ERR_LOG(0, 0, "Zookeeper:无法获得上下文.");
        return;
    }

    // 不是Watcher类型的回调，忽略它
    if (p_context->m_watcher_type == ZookeeperCtx::NOT_WATCHER)
    {
        ERR_LOG(0, 0, "Zookeeper:非Watcher类型的回调,watcher_type[%d],context[%p].",
                p_context->m_watcher_type, p_context);
        return;
    }

    // 停止，不调用用户的回调函数
    if (p_context->m_is_stop)
    {
        return;
    }

    ZookeeperManager &manager = p_context->m_zookeeper_manager;
    int32_t ret = ZOK;

    if (manager.m_zk_tid == 0)
    {
        manager.m_zk_tid = syscall(__NR_gettid);
    }

    if (zh != manager.m_zhandle)
    {
        ERR_LOG(0, 0, "严重错误:Zookeeper回调句柄[%p]与记录中的句柄[%p]不一致.", zh, manager.m_zhandle);
        return;
    }

    if (type == ZOO_SESSION_EVENT)
    {
        if (state == ZOO_CONNECTED_STATE)
        {
            // 连接建立事件
            if (manager.m_need_resume_env)
            {
                manager.ReconnectResumeEnv();
            }

            manager.m_connect_cond.notify_all();
        }
        else if (state == ZOO_EXPIRED_SESSION_STATE)
        {
            // 超时事件，重新连接，直到成功
            uint32_t retry_count = 0;
            while (true)
            {
                ++retry_count;
                ret = manager.Reconnect();
                if (ret != ZOK)
                {
                    WARN_LOG(0, 0, "第[%u]次重连失败,ret[%d],1秒后继续尝试.", retry_count, ret);
                    sleep(1);
                    continue;
                }

                // 重连之后，直接返回，因为上次连接的相关的各种句柄已经失效
                INFO_LOG(0, 0, "第[%u]次重连成功.", retry_count);
                return;
            }
        }
        else
        {
            INFO_LOG(0, 0, "Zookeeper:Session事件触发，当前状态[%d].", state);
        }
    }

    if (p_context->m_watcher_fun == NULL || *p_context->m_watcher_fun == NULL)
    {
        INFO_LOG(0, 0, "Zookeeper:回调函数为空.");
        return;
    }

    // 不自动注册Watcher或者abs_path为空，直接调用用户的Watcher即可
    if (!p_context->m_auto_reg_watcher || abs_path == NULL || *abs_path == '\0')
    {
        if (p_context->m_watcher_fun && (*p_context->m_watcher_fun) != NULL)
        {
            static_cast<void>((*p_context->m_watcher_fun)(manager, type, state, abs_path));
        }
        return;
    }

    // 要删除的Watch Type的掩码，只有这个Watcher要被删除的时候才会使用到
    uint8_t stop_watcher_type_mask = 0;

    /* 自动注册Watcher，根据注册时调用的操作类型来 */
    if (p_context->m_watcher_type == ZookeeperCtx::EXISTS)
    {
        ret = zoo_wexists(manager.GetHandler(), abs_path, &ZookeeperManager::InnerWatcher, p_context, NULL);
        if (ret != ZOK && ret != ZNONODE)
        {
            ERR_LOG(0, 0, "Zookeeper:发生错误,ret[%d],zerror[%s].", ret, zerror(ret));
            type = ZOO_NOTWATCHING_EVENT;
            p_context->m_is_stop = true;
        }
    }
    else if (p_context->m_watcher_type == ZookeeperCtx::GET)
    {
        if (type == ZOO_DELETED_EVENT)
        {
            // 节点删除，不再重新Watcher
            p_context->m_is_stop = true;
        }
        else
        {
            char buf;
            int buflen = 1;
            ret = zoo_wget(manager.GetHandler(), abs_path, &ZookeeperManager::InnerWatcher, p_context, &buf, &buflen, NULL);
            if (ret == ZNONODE)
            {
                // 节点已经被删除了，改成DELETE事件
                p_context->m_is_stop = true;
                type = ZOO_DELETED_EVENT;
            }
            else if (ret != ZOK)
            {
                ERR_LOG(0, 0, "Zookeeper:发生错误,ret[%d],zerror[%s].", ret, zerror(ret));
                type = ZOO_NOTWATCHING_EVENT;
                p_context->m_is_stop = true;
            }
            else
            {
                // Nothing
            }
        }
    }
    else if (p_context->m_watcher_type == ZookeeperCtx::GET_CHILDREN)
    {
        if (type == ZOO_DELETED_EVENT)
        {
            // 节点删除，不再重新Watcher
            p_context->m_is_stop = true;
        }
        else
        {
            // 重新注册，TODO(moon)：看是否需要把children和stat传给watcher，避免watcher中调用，看使用量，这些参数可以统一封装在一个对象里
            ScopedStringVector children;
            ret = zoo_wget_children(manager.GetHandler(), abs_path, &ZookeeperManager::InnerWatcher,
                                    p_context, &children);

            // 失败的话，发一个ZOO_NOTWATCHING_EVENT事件
            if (ret == ZNONODE)
            {
                // 节点已经被删除了，改成DELETE事件
                p_context->m_is_stop = true;
                type = ZOO_DELETED_EVENT;
            }
            else if (ret != ZOK)
            {
                ERR_LOG(0, 0, "Zookeeper:发生错误,ret[%d],zerror[%s].", ret, zerror(ret));
                p_context->m_is_stop = true;
                type = ZOO_NOTWATCHING_EVENT;
            }
            else
            {
                // Nothing
            }
        }
    }
    else if (p_context->m_watcher_type == ZookeeperCtx::GLOBAL)
    {
        unique_lock<recursive_mutex> lock(manager.m_global_watcher_path_type_lock);
        auto it = manager.m_global_watcher_path_type.find(abs_path);
        if (it == manager.m_global_watcher_path_type.end())
        {
            // 全局事件，找不到type，不再调用Watcher，直接返回
            return;
        }

        // Global类型，表示使用的是默认的Watcher，通过type和abs_path来判断使用说明方式注册
        // 如果是节点变更，优先使用Exists注册，再使用Get注册
        // 如果是节点被删除，判断是否使用Exists注册过，如果有，则只注册Exists事件，其他事件清掉
        // 如果是节点创建，使用Exists注册
        // 如果是子节点变更，使用GetChildren注册

        if (type == ZOO_CHANGED_EVENT)
        {
            // 如果是有Exists注册，优先使用Exists重注册
            if ((it->second & WATCHER_EXISTS) == WATCHER_EXISTS)
            {
                ret = manager.Exists(abs_path, NULL, 1);

                // 节点不存在，注册Watcher也是OK的
                if (ret == ZNONODE)
                {
                    ret = ZOK;
                }

                stop_watcher_type_mask = ~WATCHER_EXISTS;
            }
            else if ((it->second & WATCHER_GET) == WATCHER_GET)
            {
                // TODO 看是否要带数据给用户的Watcher
                char buf;
                int buflen = 1;
                ret = manager.Get(abs_path, &buf, &buflen, NULL, 1);
                stop_watcher_type_mask = ~WATCHER_GET;
            }
            else
            {
                // 不匹配路径对应的全局Watcher类型，停止Exists和Get类型重注册
                p_context->m_is_stop = true;
                stop_watcher_type_mask = ~WATCHER_EXISTS;
            }
        }
        else if (type == ZOO_DELETED_EVENT)
        {
            // 如果是有Exists注册，使用Exists重注册
            if ((it->second & WATCHER_EXISTS) == WATCHER_EXISTS)
            {
                // 删掉GetChildren事件
                it->second = WATCHER_EXISTS;
                ret = manager.Exists(abs_path, NULL, 1);

                // 节点不存在，注册Watcher也是OK的
                if (ret == ZNONODE)
                {
                    ret = ZOK;
                }
            }
            else
            {
                p_context->m_is_stop = true;
            }

            // 如果不重注册，则停止所有类型重注册，因为不管注册了几种Watcher，全局Watcher删除时只会触发一次
            stop_watcher_type_mask = 0;
        }
        else if (type == ZOO_CREATED_EVENT)
        {
            if ((it->second & WATCHER_EXISTS) == WATCHER_EXISTS)
            {
                ret = manager.Exists(abs_path, NULL, 1);

                // 节点不存在，注册Watcher也是OK的
                if (ret == ZNONODE)
                {
                    ret = ZOK;
                }

                stop_watcher_type_mask = ~WATCHER_EXISTS;
            }
            else if ((it->second & WATCHER_GET) == WATCHER_GET)
            {
                // 其实这里正常情况是进不来的，不过也放在这里好了
                // TODO 看是否要带数据给用户的Watcher
                char buf;
                int buflen = 1;
                ret = manager.Get(abs_path, &buf, &buflen, NULL, 1);
                stop_watcher_type_mask = ~WATCHER_GET;
            }
            else
            {
                // 不匹配路径对应的全局Watcher类型，停止Exists和Get类型重注册
                p_context->m_is_stop = true;
                stop_watcher_type_mask = ~WATCHER_EXISTS;
            }
        }
        else if (type == ZOO_CHILD_EVENT)
        {
            if ((it->second & WATCHER_GET_CHILDREN) == WATCHER_GET_CHILDREN)
            {
                // 子节点事件
                ScopedStringVector children;
                ret = manager.GetChildren(abs_path, children, 1);
                stop_watcher_type_mask = ~WATCHER_GET_CHILDREN;
            }
            else
            {
                // 不匹配路径对应的全局Watcher类型，停止GetChildren类型重注册
                p_context->m_is_stop = true;
                stop_watcher_type_mask = ~WATCHER_GET_CHILDREN;
            }
        }
        else
        {
            // 无操作，直接透传给全局Watcher
        }

        if (ret != ZOK)
        {
            ERR_LOG(0, 0, "Zookeeper:发生错误,ret[%d],zerror[%s].", ret, zerror(ret));
            type = ZOO_NOTWATCHING_EVENT;
            p_context->m_is_stop = true;
        }
    }

    // 调用用户的Watcher
    // 删除指定节点的Watcher，回调返回true或者之前流程将p_context->m_is_stop置为true表示要删除这个Watcher
    // 在get和get_children类型中，如果节点被删除了，那么也会停止重注册
    if ((*p_context->m_watcher_fun)(manager, type, state, abs_path) || p_context->m_is_stop)
    {
        if (p_context->m_watcher_type == ZookeeperCtx::GLOBAL)
        {
            // 删除指定节点的全局Watcher信息，下次仍然会触发，只是找不到了，就不调用用户的Watcher了而已
            unique_lock<recursive_mutex> lock(manager.m_global_watcher_path_type_lock);
            auto it = manager.m_global_watcher_path_type.find(abs_path);
            if (it != manager.m_global_watcher_path_type.end())
            {
                it->second &= stop_watcher_type_mask;
                if (it->second == 0)
                {
                    manager.m_global_watcher_path_type.erase(it);
                }
            }
        }
        else
        {
            // 删除指定节点的自定义Watcher
            p_context->m_is_stop = true;

            manager.DelCustomWatcher(abs_path, p_context);

            // 删除上面的流程重注册的信息，这里如果不删除的话，可能还会触发回调，但是context已经在上面被释放了
            // 虽然有多重机制保障不会出问题，但是如果原始地址仍然是一个context，还是会出问题。还可能会出现更严重的内存访问错误的问题。
            // 这里是删除原生API中回调的Watcher
            manager.DeleteWatcher(type, abs_path, p_context);
            //destroy_watcher_object_list(collectWatchers(manager.m_zhandle, type, const_cast<char *>(abs_path)));
        }
    }
}

void ZookeeperManager::InnerVoidCompletion(int rc, const void *p_zookeeper_context)
{
    ZookeeperCtx *p_context = const_cast<ZookeeperCtx *>(reinterpret_cast<const ZookeeperCtx *>(p_zookeeper_context));
    if (p_context == NULL)
    {
        ERR_LOG(0, 0, "Zookeeper:回调函数上下文为空.");
        return;
    }

    unique_ptr<ZookeeperCtx> up_context(p_context);

    ZookeeperManager &manager = up_context->m_zookeeper_manager;

    if (rc == ZOK && !up_context->m_ephemeral_path.empty())
    {
        unique_lock<recursive_mutex> phemeral_node_info_lock(manager.m_ephemeral_node_info_lock);
        if (up_context->m_ephemeral_info == NULL)
        {
            // 这种情况是删除临时节点
            manager.m_ephemeral_node_info.erase(up_context->m_ephemeral_path);
        }
        else
        {
            // 这种情况是修改临时节点信息，比如ASetAcl中的操作
            manager.m_ephemeral_node_info[up_context->m_ephemeral_path] = *up_context->m_ephemeral_info;
        }
    }

    if (up_context->m_void_completion_fun != NULL && *up_context->m_void_completion_fun != NULL)
    {
        (*up_context->m_void_completion_fun)(manager, rc);
    }
}

void ZookeeperManager::InnerStatCompletion(int rc, const Stat *stat, const void *p_zookeeper_context)
{
    ZookeeperCtx *p_context = const_cast<ZookeeperCtx *>(reinterpret_cast<const ZookeeperCtx *>(p_zookeeper_context));
    if (p_context == NULL)
    {
        ERR_LOG(0, 0, "Zookeeper:回调函数上下文为空.");
        return;
    }

    unique_ptr<ZookeeperCtx> up_context(p_context);

    ZookeeperManager &manager = up_context->m_zookeeper_manager;

    if (rc == ZOK)
    {
        // 处理临时节点
        if (up_context->m_ephemeral_info != NULL && !up_context->m_ephemeral_path.empty())
        {
            unique_lock<recursive_mutex> phemeral_node_info_lock(manager.m_ephemeral_node_info_lock);
            manager.m_ephemeral_node_info[up_context->m_ephemeral_path] = *up_context->m_ephemeral_info;
        }
    }

    // Exist还要额外考虑ZNONODE返回值
    if (rc == ZOK || (rc == ZNONODE
                      && (up_context->m_global_watcher_add_type == WATCHER_EXISTS
                          || (up_context->m_custom_watcher_context != NULL
                              && up_context->m_custom_watcher_context->m_watcher_type == ZookeeperCtx::EXISTS))))
    {
        // 处理Watcher
        manager.ProcAsyncWatcher(*up_context);
    }

    if (up_context->m_stat_completion_fun != NULL && *up_context->m_stat_completion_fun != NULL)
    {
        (*up_context->m_stat_completion_fun)(manager, rc, stat);
    }
}

void ZookeeperManager::InnerDataCompletion(int rc, const char *value, int value_len,
                                           const Stat *stat, const void *p_zookeeper_context)
{
    ZookeeperCtx *p_context = const_cast<ZookeeperCtx *>(reinterpret_cast<const ZookeeperCtx *>(p_zookeeper_context));
    if (p_context == NULL)
    {
        ERR_LOG(0, 0, "Zookeeper:回调函数上下文为空.");
        return;
    }

    unique_ptr<ZookeeperCtx> up_context(p_context);

    ZookeeperManager &manager = up_context->m_zookeeper_manager;

    if (rc == ZOK)
    {
        // 处理Watcher
        manager.ProcAsyncWatcher(*up_context);
    }

    if (up_context->m_data_completion_fun != NULL && *up_context->m_data_completion_fun != NULL)
    {
        (*up_context->m_data_completion_fun)(manager, rc, value, value_len, stat);
    }
}

void ZookeeperManager::InnerStringsCompletion(int rc, const String_vector * strings,
                                              const void *p_zookeeper_context)
{
    ZookeeperCtx *p_context = const_cast<ZookeeperCtx *>(reinterpret_cast<const ZookeeperCtx *>(p_zookeeper_context));
    if (p_context == NULL)
    {
        ERR_LOG(0, 0, "Zookeeper:回调函数上下文为空.");
        return;
    }

    unique_ptr<ZookeeperCtx> up_context(p_context);

    ZookeeperManager &manager = up_context->m_zookeeper_manager;

    if (rc == ZOK)
    {
        // 处理Watcher
        manager.ProcAsyncWatcher(*up_context);
    }

    if (up_context->m_strings_stat_completion_fun != NULL && *up_context->m_strings_stat_completion_fun != NULL)
    {
        (*up_context->m_strings_stat_completion_fun)(manager, rc, strings, NULL);
    }
}

void ZookeeperManager::InnerStringsStatCompletion(int rc, const String_vector *strings,
                                                  const Stat *stat, const void *p_zookeeper_context)
{
    ZookeeperCtx *p_context = const_cast<ZookeeperCtx *>(reinterpret_cast<const ZookeeperCtx *>(p_zookeeper_context));
    if (p_context == NULL)
    {
        ERR_LOG(0, 0, "Zookeeper:回调函数上下文为空.");
        return;
    }

    unique_ptr<ZookeeperCtx> up_context(p_context);

    ZookeeperManager &manager = up_context->m_zookeeper_manager;

    if (rc == ZOK)
    {
        // 处理Watcher
        manager.ProcAsyncWatcher(*up_context);
    }

    if (up_context->m_strings_stat_completion_fun != NULL && *up_context->m_strings_stat_completion_fun != NULL)
    {
        (*up_context->m_strings_stat_completion_fun)(manager, rc, strings, stat);
    }
}

void ZookeeperManager::InnerStringCompletion(int rc, const char *value, const void *p_zookeeper_context)
{
    ZookeeperCtx *p_context = const_cast<ZookeeperCtx *>(reinterpret_cast<const ZookeeperCtx *>(p_zookeeper_context));
    if (p_context == NULL)
    {
        ERR_LOG(0, 0, "Zookeeper:回调函数上下文为空.");
        return;
    }

    unique_ptr<ZookeeperCtx> up_context(p_context);

    ZookeeperManager &manager = up_context->m_zookeeper_manager;

    // 成功调用，处理临时节点
    if (rc == ZOK && up_context->m_ephemeral_info != NULL && !up_context->m_ephemeral_path.empty())
    {
        unique_lock<recursive_mutex> phemeral_node_info_lock(manager.m_ephemeral_node_info_lock);
        manager.m_ephemeral_node_info[up_context->m_ephemeral_path] = *up_context->m_ephemeral_info;
    }

    if (up_context->m_string_completion_fun != NULL && *up_context->m_string_completion_fun != NULL)
    {
        (*up_context->m_string_completion_fun)(manager, rc, value);
    }
}

void ZookeeperManager::InnerAclCompletion(int rc, ACL_vector *acl, Stat *stat, const void *p_zookeeper_context)
{
    ZookeeperCtx *p_context = const_cast<ZookeeperCtx *>(reinterpret_cast<const ZookeeperCtx *>(p_zookeeper_context));
    if (p_context == NULL)
    {
        ERR_LOG(0, 0, "Zookeeper:回调函数上下文为空.");
        return;
    }

    unique_ptr<ZookeeperCtx> up_context(p_context);

    ZookeeperManager &manager = up_context->m_zookeeper_manager;

    if (up_context->m_acl_completion_fun != NULL && *up_context->m_acl_completion_fun != NULL)
    {
        (*up_context->m_acl_completion_fun)(manager, rc, acl, stat);
    }
}

void ZookeeperManager::InnerMultiCompletion(int rc, const void *p_zookeeper_context)
{
    ZookeeperCtx *p_context = const_cast<ZookeeperCtx *>(reinterpret_cast<const ZookeeperCtx *>(p_zookeeper_context));
    if (p_context == NULL)
    {
        ERR_LOG(0, 0, "Zookeeper:回调函数上下文为空.");
        return;
    }

    unique_ptr<ZookeeperCtx> up_context(p_context);

    ZookeeperManager &manager = up_context->m_zookeeper_manager;

    // 处理临时节点，这里可能会出现部分成功部分失败的情况
    manager.ProcMultiEphemeralNode(up_context->m_multi_ops->m_multi_ops, *up_context->m_multi_results);

    if (up_context->m_multi_completion_fun != NULL && *up_context->m_multi_completion_fun != NULL)
    {
        (*up_context->m_multi_completion_fun)(manager, rc, up_context->m_multi_ops, up_context->m_multi_results);
    }
}

void ZookeeperManager::AddCustomWatcher(const string &abs_path, shared_ptr<ZookeeperCtx> watcher_context)
{
    unique_lock<recursive_mutex> custom_watcher_contexts_lock(m_custom_watcher_contexts_lock);
    auto find_its = m_custom_watcher_contexts.equal_range(abs_path);
    for (auto it = find_its.first; it != find_its.second; ++it)
    {
        if (it->second == watcher_context)
        {
            return;
        }
    }

    m_custom_watcher_contexts.insert(make_pair(abs_path, watcher_context));
}

void ZookeeperManager::DelCustomWatcher(const string &abs_path, const ZookeeperCtx *watcher_context)
{
    unique_lock<recursive_mutex> custom_watcher_contexts_lock(m_custom_watcher_contexts_lock);
    auto find_its = m_custom_watcher_contexts.equal_range(abs_path);
    for (auto it = find_its.first; it != find_its.second; ++it)
    {
        if (it->second.get() == watcher_context)
        {
            // 这里erase之后，it不能再使用，后面如果要修改，需要注意
            m_custom_watcher_contexts.erase(it);
            return;
        }
    }
}

void ZookeeperManager::ReconnectResumeEnv()
{
    int32_t ret;

    /* 重新注册所有的Watcher */
    // 注册全局Watcher
    INFO_LOG(0, 0, "Zookeeper:开始重新注册全局Watcher.");

    unique_lock<recursive_mutex> global_watcher_path_type_lock(m_global_watcher_path_type_lock);
    for (auto it = m_global_watcher_path_type.begin(); it != m_global_watcher_path_type.end(); ++it)
    {
        ret = ZOK;

        INFO_LOG(0, 0, "Zookeeper:重新注册全局Watcher,路径[%s],类型[%u].", it->first.c_str(), it->second);
        if (it->first.empty() || it->first[0] != '/')
        {
            ERR_LOG(0, 0, "Zookeeper:无效的路径[%s].", it->first.c_str());
            continue;
        }

        // exists和get 二选一，优先exists
        if ((it->second & WATCHER_EXISTS) == WATCHER_EXISTS)
        {
            ret = Exists(it->first.c_str(), NULL, 1);
            if (ret == ZNONODE)
            {
                ret = ZOK;
            }
        }
        else if ((it->second & WATCHER_GET) == WATCHER_GET)
        {
            char buf;
            int buflen = 1;
            ret = Get(it->first.c_str(), &buf, &buflen, NULL, 1);
        }
        else
        {
            // Nothing
        }

        // 注册错误，发个NOWATCH事件？TODO(moon)，短信通知
        if (ret != ZOK)
        {
            ERR_LOG(0, 0, "严重错误：Zookeeper:重新注册全局Watcher发生错误:ret[%d],zerror[%s].",
                    ret, zerror(ret));
        }

        if ((it->second & WATCHER_GET_CHILDREN) == WATCHER_GET_CHILDREN)
        {
            ScopedStringVector children;
            ret = GetChildren(it->first.c_str(), children, 1);

            // 注册错误，发个NOWATCH事件？TODO(moon)，短信通知
            if (ret != ZOK)
            {
                ERR_LOG(0, 0, "严重错误：Zookeeper:重新注册全局Watcher发生错误:ret[%d],zerror[%s].",
                        ret, zerror(ret));
            }
        }
    }
    global_watcher_path_type_lock.unlock();

    // 重新注册自定义Watcher
    INFO_LOG(0, 0, "Zookeeper:开始重新注册自定义Watcher.");
    unique_lock<recursive_mutex> custom_watcher_contexts_lock(m_custom_watcher_contexts_lock);
    for (auto it = m_custom_watcher_contexts.begin(); it != m_custom_watcher_contexts.end(); ++it)
    {
        ret = ZOK;

        INFO_LOG(0, 0, "Zookeeper:重新注册全局Watcher,路径[%s],类型[%u].",
                 it->first.c_str(), it->second->m_watcher_type);
        if (it->second->m_watcher_type == ZookeeperCtx::EXISTS)
        {
            ret = zoo_wexists(m_zhandle, it->first.c_str(),
                              &ZookeeperManager::InnerWatcher, it->second.get(), NULL);
        }
        else if (it->second->m_watcher_type == ZookeeperCtx::GET)
        {
            char buf;
            int buflen = 1;
            ret = zoo_wget(m_zhandle, it->first.c_str(), &ZookeeperManager::InnerWatcher, it->second.get(),
                           &buf, &buflen, NULL);
        }
        else if (it->second->m_watcher_type == ZookeeperCtx::GET_CHILDREN)
        {
            ScopedStringVector children;
            ret = zoo_wget_children(m_zhandle, it->first.c_str(), &ZookeeperManager::InnerWatcher,
                                    it->second.get(), &children);
        }
        else
        {
            WARN_LOG(0, 0, "Zookeeper:无效的Watcher类型[%d].", it->second->m_watcher_type);
        }

        // 注册错误，发个NOWATCH事件？TODO(moon)，短信通知
        if (ret != ZOK)
        {
            ERR_LOG(0, 0, "严重错误：Zookeeper:重新注册自定义Watcher发生错误:ret[%d],zerror[%s].",
                    ret, zerror(ret));
        }
    }
    custom_watcher_contexts_lock.unlock();

    // 重新注册所有的临时节点
    unique_lock<recursive_mutex> phemeral_node_info_lock(m_ephemeral_node_info_lock);
    for (auto it = m_ephemeral_node_info.begin(); it != m_ephemeral_node_info.end(); ++it)
    {
        // 先尝试创建一下临时节点，如果失败提示节点不存在，表示父节点不存在，创建父节点后重试一次
        ret = Create(it->first.c_str(), it->second.Data.data(), NULL, &it->second.Acl, it->second.Flags);
        if (ret == ZNONODE)
        {
            // 递归创建父节点
            auto last_slash_pos = it->first.rfind('/');
            if (last_slash_pos == string::npos)
            {
                ERR_LOG(0, 0, "无效的临时节点路径[%s],找不到'/',跳过.", it->first.c_str());
                continue;
            }

            if (last_slash_pos == 0)
            {
                ERR_LOG(0, 0, "根节点下无法创建临时节点[%s],原因未知，这个错误不该发生,跳过.", it->first.c_str());
                continue;
            }

            string parent_path = it->first.substr(0, last_slash_pos);
            ret = CreatePathRecursion(parent_path);
            if (ret != ZOK)
            {
                // 重试一次
                ret = CreatePathRecursion(parent_path);
                if (ret != ZOK)
                {
                    // TODO(moon)：短信通知
                    ERR_LOG(0, 0, "严重错误：创建临时节点[%s]父节点[%s]失败,ret[%d],临时节点无法创建.",
                            it->first.c_str(), parent_path.c_str(), ret);
                    continue;
                }
            }

            // 重新创建临时节点，错误码在外层判断
            ret = Create(it->first.c_str(), it->second.Data.data(), NULL, &it->second.Acl, it->second.Flags);
        }

        if (ret != ZOK)
        {
            // 重试一次
            ret = Create(it->first.c_str(), it->second.Data.data(), NULL, &it->second.Acl, it->second.Flags);
            if (ret != ZOK)
            {
                // TODO(moon)：短信通知
                ERR_LOG(0, 0, "严重错误：创建临时节点[%s]重试还失败,ret[%d]，临时节点无法创建.", it->first.c_str(), ret);
                continue;
            }
        }
    }

    m_need_resume_env = false;

    phemeral_node_info_lock.unlock();
}

void ZookeeperManager::ProcMultiEphemeralNode(const vector<zoo_op> &multi_ops,
                                              const vector<zoo_op_result_t> &multi_result)
{
    // 处理临时节点，这里可能会出现部分调用成功，部分调用失败的情况，目前只把成功的写到临时节点数据中
    unique_lock<recursive_mutex> phemeral_node_info_lock(m_ephemeral_node_info_lock);
    auto result_it = multi_result.begin();
    for (auto zoo_op_it = multi_ops.begin(); zoo_op_it != multi_ops.end() && result_it != multi_result.end();
         ++zoo_op_it, ++result_it)
    {
        if (result_it->err != ZOK)
        {
            // 跳过失败的操作
            continue;
        }

        if (zoo_op_it->type == ZOO_CREATE_OP && (zoo_op_it->create_op.flags & ZOO_EPHEMERAL))
        {
            // 如果是临时节点，添加到临时节点列表中。
            m_ephemeral_node_info[zoo_op_it->create_op.path].Acl = *zoo_op_it->create_op.acl;
            m_ephemeral_node_info[zoo_op_it->create_op.path].Data.assign(zoo_op_it->create_op.data,
                                                                         zoo_op_it->create_op.datalen);
            m_ephemeral_node_info[zoo_op_it->create_op.path].Flags = zoo_op_it->create_op.flags;
        }
        else if (zoo_op_it->type == ZOO_DELETE_OP
                 && m_ephemeral_node_info.find(zoo_op_it->create_op.path) != m_ephemeral_node_info.end())
        {
            m_ephemeral_node_info.erase(zoo_op_it->create_op.path);
        }
        else if (zoo_op_it->type == ZOO_SETDATA_OP
                 && m_ephemeral_node_info.find(zoo_op_it->create_op.path) != m_ephemeral_node_info.end())
        {
            // 如果在临时节点列表中找到，修改数据
            m_ephemeral_node_info[zoo_op_it->create_op.path].Data.assign(zoo_op_it->create_op.data,
                                                                         zoo_op_it->create_op.datalen);
        }
        else
        {
            // Nothing
        }
    }
}

void ZookeeperManager::ProcAsyncWatcher(ZookeeperCtx &context)
{
    if (!context.m_watch_path.empty())
    {
        if (context.m_global_watcher_add_type != 0)
        {
            // 全局Watcher
            unique_lock<recursive_mutex> global_watcher_path_type_lock(m_global_watcher_path_type_lock);
            m_global_watcher_path_type[context.m_watch_path] |= context.m_global_watcher_add_type;
        }
        else if (context.m_custom_watcher_context != NULL)
        {
            // 自定义Watcher
            AddCustomWatcher(context.m_watch_path, context.m_custom_watcher_context);
        }
        else
        {
            ERR_LOG(0, 0, "没有自定义Watcher也不是全局Watcher，这里一定是有问题了,path[%s].",
                    context.m_watch_path.c_str());
        }
    }
}

int32_t MultiOps::GetOp(uint32_t index, zoo_op *&op)
{
    if (index < m_multi_ops.size())
    {
        return ZBADARGUMENTS;
    }

    op = &m_multi_ops[index];
    return ZOK;
}

void MultiOps::AddCreateOp(const string &path, const char *value, int valuelen,
                           const ACL_vector *acl /*= &ZOO_OPEN_ACL_UNSAFE*/, int flags /*= 0*/,
                           uint32_t max_real_path_size /*= 128*/)
{
    zoo_op op;
    string abs_path = mp_zk_manager == NULL ? path : mp_zk_manager->ChangeToAbsPath(path);
    shared_ptr<string> curr_path = make_shared<string>(move(abs_path));
    shared_ptr<string> curr_buffer = make_shared<string>(value, valuelen);

    if (max_real_path_size > 0)
    {
        shared_ptr<string> real_path = make_shared<string>(max_real_path_size, '\0');
        zoo_create_op_init(&op, curr_path->c_str(), curr_buffer->data(), curr_buffer->size(),
                           acl, flags, &(*real_path)[0], real_path->size());
        m_inner_strings.push_back(real_path);
    }
    else
    {
        zoo_create_op_init(&op, curr_path->c_str(), curr_buffer->data(), curr_buffer->size(),
                           acl, flags, NULL, 0);
    }

    m_multi_ops.push_back(op);
    m_inner_strings.push_back(curr_path);
    m_inner_strings.push_back(curr_buffer);
}

void MultiOps::AddCreateOp(const string &path, const string &value,
                           const ACL_vector *acl /*= &ZOO_OPEN_ACL_UNSAFE*/, int flags /*= 0*/,
                           uint32_t max_real_path_size /*= 128*/)
{
    AddCreateOp(path, value.data(), value.size(), acl, flags, max_real_path_size);
}

void MultiOps::AddDeleteOp(const string &path, int version)
{
    zoo_op op;
    string abs_path = mp_zk_manager == NULL ? path : mp_zk_manager->ChangeToAbsPath(path);
    shared_ptr<string> curr_path = make_shared<string>(move(abs_path));

    zoo_delete_op_init(&op, curr_path->c_str(), version);

    m_multi_ops.push_back(op);
    m_inner_strings.push_back(curr_path);
}

void MultiOps::AddSetOp(const string &path, const char *buffer, int buflen, int version, bool need_stat /*= false*/)
{
    zoo_op op;
    string abs_path = mp_zk_manager == NULL ? path : mp_zk_manager->ChangeToAbsPath(path);
    shared_ptr<string> curr_path = make_shared<string>(move(abs_path));
    shared_ptr<string> curr_buffer = make_shared<string>(buffer, buflen);
    if (need_stat)
    {
        shared_ptr<string> stat_buf = make_shared<string>(sizeof(Stat), '\0');
        zoo_set_op_init(&op, curr_path->c_str(), curr_buffer->data(), curr_buffer->size(),
                        version, reinterpret_cast<Stat *>(const_cast<char *>(stat_buf->data())));
        m_inner_strings.push_back(stat_buf);
    }
    else
    {
        zoo_set_op_init(&op, curr_path->c_str(), curr_buffer->data(), curr_buffer->size(), version, NULL);
    }

    m_multi_ops.push_back(op);
    m_inner_strings.push_back(curr_path);
    m_inner_strings.push_back(curr_buffer);
}

void MultiOps::AddSetOp(const string &path, const string &buffer, int version, bool need_stat /*= false*/)
{
    AddSetOp(path, buffer.data(), buffer.size(), version, need_stat);
}

void MultiOps::AddCheckOp(const string &path, int version)
{
    zoo_op op;
    string abs_path = mp_zk_manager == NULL ? path : mp_zk_manager->ChangeToAbsPath(path);
    shared_ptr<string> curr_path = make_shared<string>(move(abs_path));

    zoo_check_op_init(&op, curr_path->c_str(), version);

    m_multi_ops.push_back(op);
    m_inner_strings.push_back(curr_path);
}

}
