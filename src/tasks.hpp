#pragma once

#include "mutex.hpp"
#include "catalog.hpp"
#include "net.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

enum TaskState { TS_QUEUED, TS_RUNNING, TS_DONE, TS_FAILED, TS_CANCELED };

struct Task {
    Entry entry;
    std::vector<Entry> deps;
    std::atomic<int> state{TS_QUEUED};
    net::Progress progress;
    Mutex m;
    std::string stage;
    std::string error;
    std::string note;
    uint64_t sample_time = 0;
    uint64_t sample_bytes = 0;
    double speed = 0;
    bool reboot = false;
    bool restart = false;
    bool staged = false;
    bool notified = false;
    bool synced = false;

    std::string get_stage();
    std::string get_error();
    std::string get_note();
    void set_stage(const std::string& s);
};

class Tasks {
public:
    void start(Installed* installed);
    void stop();
    std::shared_ptr<Task> add(const Entry& e, const std::vector<Entry>& deps = {});
    std::shared_ptr<Task> find(const Entry& e);
    std::vector<std::shared_ptr<Task>> list();
    void clear_finished();
    int active();

private:
    Mutex m;
    std::vector<std::shared_ptr<Task>> tasks;
    Installed* installed = nullptr;
    int thread = -1;
    std::atomic<bool> quit{false};
    static int worker(unsigned int, void* arg);
    void run(Task& t);
};

std::string tai_dir();
std::string bg_file(const std::string& url);
bool data_installed(const Entry& e, bool probe);
std::string first_link(const std::string& primary);
bool plugin_present(const Entry& plugin);
bool tai_add_plugin(const std::string& section, const std::string& path, std::string& err);
bool tai_remove_plugin(const std::string& path);
