#include "MapBasedGlobalLockImpl.h"

#include <mutex>
#include <iostream>


namespace Afina {
namespace Backend {

Entry::Entry(Entry *prev, Entry *next, const std::string& key, const std::string& val)
        : _prev(prev), _next(next), _key(key), _val(val){};


List::~List() {
    Entry *iter = _head;
    while(iter->_next != nullptr){
        iter = iter->_next;
        delete iter->_prev;
    }
    delete iter;
}

bool List::Put(const std::string& key, const std::string& value, Entry *& entry){
    Entry *new_entry = new Entry(nullptr, _head, key, value);

    if(_head != nullptr) {
        _head->_prev = new_entry;
    } else {
        _tail = new_entry;
    }

    _head = new_entry;
    entry = new_entry;

    return true;
}

bool List::ToForward(Entry *entry) {
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

bool List::Delete(Entry *entry) {
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
bool MapBasedGlobalLockImpl::ReleaseSpace(size_t amount) {
    if(amount > _max_size){
        return false;
    }
    while(_current_size + amount > _max_size){
        Delete(_entries._tail->_key);
    }
    return true;
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::Put(const std::string &key, const std::string &value) {
    auto element = _backend.find(key);
    size_t size_delta = 0;

    if(element == _backend.end()){
        size_delta = key.size() + value.size();

        if(!ReleaseSpace(size_delta)){
            return false;
        }

        Entry *new_entry = nullptr;
        auto new_element = _backend.emplace(key, new_entry);

        _current_size = _current_size + size_delta;
        _entries.Put(new_element.first->first, value, new_entry);
        new_element.first->second = new_entry;
    } else {
        Entry *entry = element->second;
        size_delta = value.size() - entry->_val.size();

        if(!ReleaseSpace(size_delta)){
            return false;
        }

        _current_size = _current_size + size_delta;
        entry->_val = value;
        _entries.ToForward(entry);
    }
    return true;
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::PutIfAbsent(const std::string &key, const std::string &value) {
    auto element = _backend.find(key);
    size_t size_delta = 0;

    if(element == _backend.end()){
        size_delta = key.size() + value.size();

        if(!ReleaseSpace(size_delta)){
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
    auto element = _backend.find(key);
    size_t size_delta = 0;

    if(element == _backend.end()){
        return false;
    } else {
        Entry *entry = element->second;
        size_delta = value.size() - entry->_val.size();

        if(!ReleaseSpace(size_delta)){
            return false;
        }

        _current_size = _current_size + size_delta;
        _entries.ToForward(entry);

        return true;
    }
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::Delete(const std::string &key) {
    auto element = _backend.find(key);

    if(element == _backend.end()){
        return false;
    } else {
        _current_size = _current_size - (element->first.size() + element->second->_val.size());
        _entries.Delete(element->second);
        _backend.erase(element);

        return true;
    }

}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::Get(const std::string &key, std::string &value) const {
    auto element = _backend.find(key);

    if(element == _backend.end()){
        return false;
    } else {
        Entry *entry = element->second;
        value = entry->_val;
        _entries.ToForward(entry);

        return true;
    }
}

} // namespace Backend
} // namespace Afina
