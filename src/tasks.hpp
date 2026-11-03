#pragma once

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
    std::mutex m;
    std::string stage;
    std::string error;
    std::string note;
    uint64_t sample_time = 0;
    uint64_t sample_bytes = 0;
    double speed = 0;
    bool reboot = false;
    bool notified = false;

    std::string get_stage();
    std::string get_error();
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
    std::mutex m;
    std::vector<std::shared_ptr<Task>> tasks;
    Installed* installed = nullptr;
    int thread = -1;
    std::atomic<bool> quit{false};
    static int worker(unsigned int, void* arg);
    void run(Task& t);
};

std::string tai_dir();
bool plugin_present(const Entry& plugin);
