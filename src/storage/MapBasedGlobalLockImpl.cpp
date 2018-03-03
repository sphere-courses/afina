#include "MapBasedGlobalLockImpl.h"

#include <iostream>


namespace Afina {
namespace Backend {

// See MapBasedGlobalLockImpl.h
Entry::Entry(Entry *prev, Entry *next, const std::string& key, const std::string& val)
        : _prev(prev), _next(next), _key(key), _value(val) {};

// See MapBasedGlobalLockImpl.h
std::string& Entry::GetValue(){
    return _value;
}

// See MapBasedGlobalLockImpl.h
const std::string& Entry::GetKey() const{
    return _key;
}


// See MapBasedGlobalLockImpl.h
ListOnMap::~ListOnMap() {
    Entry *iter = _head;
    while(iter->_next != nullptr){
        iter = iter->_next;
        delete iter->_prev;
    }
    delete iter;
}

// See MapBasedGlobalLockImpl.h
bool ListOnMap::Put(const std::string& key, const std::string& value, Entry *& entry){
    auto new_entry = new Entry(nullptr, _head, key, value);

    if(_head != nullptr) {
        _head->_prev = new_entry;
    } else {
        _tail = new_entry;
    }

    _head = new_entry;
    entry = new_entry;

    return true;
}

// See MapBasedGlobalLockImpl.h
bool ListOnMap::ToForward(Entry *entry) {
    Entry *next = entry->_next;
    Entry *prev = entry->_prev;

    if(prev != nullptr) {
        prev->_next = next;
    } else {
        return true;
    }

    if(next != nullptr) {
        next->_prev = prev;
    } else {
        _tail = prev;
    }

    entry->_prev = nullptr;
    entry->_next = _head;
    _head->_prev = entry;
    _head = entry;

    return true;
};

// See MapBasedGlobalLockImpl.h
bool ListOnMap::Delete(Entry *entry) {
    Entry *next = entry->_next;
    Entry *prev = entry->_prev;

    if(prev != nullptr) {
        prev->_next = next;
    } else {
        _head = next;
    }

    if(next != nullptr) {
        next->_prev = prev;
    } else {
        _tail = prev;
    }

    delete entry;

    return true;
}


// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::_release_space(size_t amount) {
    // No lock need because _release_space called from critical sections only

    if(amount > _max_size){
        return false;
    }
    while(_current_size + amount > _max_size){
        _lock_free_delete(_entries._tail->GetKey());
    }
    return true;
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::Put(const std::string &key, const std::string &value) {
    std::lock_guard<std::mutex> lock(_backend_mutex);

    auto element = _backend.find(key);
    size_t size_delta = 0;

    if(element == _backend.end()){
        size_delta = key.size() + value.size();

        if(!_release_space(size_delta)){
            return false;
        }

        Entry *new_entry = nullptr;
        auto new_element = _backend.emplace(key, new_entry);

        _current_size = _current_size + size_delta;
        _entries.Put(new_element.first->first, value, new_entry);
        new_element.first->second = new_entry;
    } else {
        Entry *entry = element->second;
        size_delta = value.size() - entry->GetValue().size();

        if(!_release_space(size_delta)){
            return false;
        }

        _current_size = _current_size + size_delta;
        entry->GetValue() = value;
        _entries.ToForward(entry);
    }
    return true;
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::PutIfAbsent(const std::string &key, const std::string &value) {
    std::lock_guard<std::mutex> lock(_backend_mutex);

    auto element = _backend.find(key);
    size_t size_delta = 0;

    if(element == _backend.end()){
        size_delta = key.size() + value.size();

        if(!_release_space(size_delta)){
            return false;
        }

        Entry *new_entry = nullptr;
        auto new_element = _backend.emplace(key, new_entry);

        _current_size = _current_size + size_delta;
        _entries.Put(new_element.first->first, value, new_entry);
        new_element.first->second = new_entry;

        return true;
    } else {
        return false;
    }
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::Set(const std::string &key, const std::string &value) {
    std::lock_guard<std::mutex> lock(_backend_mutex);

    auto element = _backend.find(key);
    size_t size_delta = 0;

    if(element == _backend.end()){
        return false;
    } else {
        Entry *entry = element->second;
        size_delta = value.size() - entry->GetValue().size();

        if(!_release_space(size_delta)){
            return false;
        }

        _current_size = _current_size + size_delta;
        _entries.ToForward(entry);

        return true;
    }
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::Delete(const std::string &key) {
    std::lock_guard<std::mutex> lock(_backend_mutex);

    _lock_free_delete(key);
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::_lock_free_delete(const std::string &key) {
    auto element = _backend.find(key);

    if(element == _backend.end()){
        return false;
    } else {
        _current_size = _current_size - (element->first.size() + element->second->GetValue().size());
        _entries.Delete(element->second);
        _backend.erase(element);

        return true;
    }
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::Get(const std::string &key, std::string &value) const {
    std::lock_guard<std::mutex> lock(_backend_mutex);

    auto element = _backend.find(key);

    if(element == _backend.end()){
        return false;
    } else {
        Entry *entry = element->second;
        value = entry->GetValue();
        _entries.ToForward(entry);

        return true;
    }
}

} // namespace Backend
} // namespace Afina