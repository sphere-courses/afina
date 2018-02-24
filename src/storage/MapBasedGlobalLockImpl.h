#ifndef AFINA_STORAGE_MAP_BASED_GLOBAL_LOCK_IMPL_H
#define AFINA_STORAGE_MAP_BASED_GLOBAL_LOCK_IMPL_H

#include <unordered_map>
#include <mutex>
#include <string>

#include <afina/Storage.h>

namespace Afina {
namespace Backend {

class Entry;
class List;
class MapBasedGlobalLockImpl;

class Entry {
public:
    Entry(Entry *prev, Entry *next, const std::string& key, const std::string& val);
    ~Entry(){};

private:
    friend List;
    friend MapBasedGlobalLockImpl;

    Entry *_prev = nullptr;
    Entry *_next = nullptr;
    const std::string& _key;
    std::string _val;
};


class List{
public:
    List(){};
    ~List();

    bool Put(const std::string& key, const std::string& value, Entry *& entry);

    bool ToForward(Entry *entry);

    bool Delete(Entry *entry);

private:
    friend MapBasedGlobalLockImpl;

    Entry *_head = nullptr;
    Entry *_tail = nullptr;
};

/**
 * # Map based implementation with global lock
 *
 *
 */
class MapBasedGlobalLockImpl : public Afina::Storage {
public:
    explicit MapBasedGlobalLockImpl(size_t max_size = 1024) : _max_size(max_size) {}
    ~MapBasedGlobalLockImpl() {}

    // Implements Afina::Storage interface
    bool Put(const std::string &key, const std::string &value) override;

    // Implements Afina::Storage interface
    bool PutIfAbsent(const std::string &key, const std::string &value) override;

    // Implements Afina::Storage interface
    bool Set(const std::string &key, const std::string &value) override;

    // Implements Afina::Storage interface
    bool Delete(const std::string &key) override;

    // Implements Afina::Storage interface
    bool Get(const std::string &key, std::string &value) const override;

    bool AdjustSize();

private:
    size_t _max_size, _current_size = 0;
    std::unordered_map<std::string, Entry *> _backend;
    mutable List _entries;
};

} // namespace Backend
} // namespace Afina

#endif // AFINA_STORAGE_MAP_BASED_GLOBAL_LOCK_IMPL_H
