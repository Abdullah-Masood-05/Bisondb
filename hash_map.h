#include <iostream>
#include <cassert>
#include <string>
#include <fstream>

#define INF INT_MAX

template <class k, class T>
class hash_map
{
private:
    struct HashItem
    {
        k key;
        T value;
        short status = 0;
    };

    HashItem *hashArray;
    int capacity;
    int currentElements;

    int hashFunction(const k &key) const
    {
        // Simple hash function for demonstration purposes
        return static_cast<int>(key[0]) % capacity;
    }

    int collide(int i, int j) const
    {
        return (i + j) % capacity;
    }

    void doubleCapacity()
    {
        HashItem *temp = hashArray;
        int oldCapacity = capacity;

        capacity = capacity * 2;
        hashArray = new HashItem[capacity];
        currentElements = 0;

        for (int i = 0; i < oldCapacity; ++i)
        {
            if (temp[i].status == 2)
            {
                int idx = hashFunction(temp[i].key);
                int j = 0;
                while (hashArray[idx].status == 2)
                {
                    idx = collide(idx, j);
                    ++j;
                }
                hashArray[idx].status = 2;
                hashArray[idx].key = temp[i].key;
                hashArray[idx].value = temp[i].value;
                ++currentElements;
            }
        }
        delete[] temp;
    }

    int getNextCandidateIndex(const k &key, int i) const
    {
        return (hashFunction(key) + i) % capacity;
    }

public:
    hash_map(int cap = 10) : currentElements(0), capacity(cap)
    {
        hashArray = new HashItem[capacity];
        for (int i = 0; i < capacity; i++)
            hashArray[i].status = 0;
    }

    ~hash_map()
    {
        delete[] hashArray;
    }

    T &operator[](const k &key)
    {
        int index = hashFunction(key);
        int i = 0;
        while (hashArray[index].status != 0 && hashArray[index].key != key)
        {
            index = getNextCandidateIndex(key, i);
            i++;
        }
        return hashArray[index].value;
    }

    const T &operator[](const k &key) const
    {
        int index = hashFunction(key);
        int i = 0;
        while (hashArray[index].status != 0 && hashArray[index].key != key)
        {
            index = getNextCandidateIndex(key, i);
            i++;
        }
        return hashArray[index].value;
    }

    T *find(const k &key)
    {
        int index = hashFunction(key);
        int i = 0;
        while (hashArray[index].status != 0 && hashArray[index].key != key)
        {
            index = getNextCandidateIndex(key, i);
            i++;
        }
        if (hashArray[index].status == 0)
            return nullptr;
        return &hashArray[index].value;
    }

    void insert(const std::pair<k, T> &entry)
    {
        int index = hashFunction(entry.first);
        int i = 0;
        while ((hashArray[index].status != 0) && (hashArray[index].key != entry.first))
        {
            index = getNextCandidateIndex(entry.first, i);
            i++;
        }
        hashArray[index].key = entry.first;
        hashArray[index].value = entry.second;
        hashArray[index].status = 2;
        currentElements++;
        if (currentElements >= capacity * 0.75)
            doubleCapacity();
    }

    bool erase(const k &key)
    {
        int index = hashFunction(key);
        int i = 0;
        while (hashArray[index].status != 0 && hashArray[index].key != key)
        {
            index = getNextCandidateIndex(key, i);
            i++;
        }
        if (hashArray[index].status == 0)
            return false;
        hashArray[index].status = 0;
        currentElements--;
        return true;
    }

    int size()
    {
        return currentElements;
    }

    void print()
    {
        for (int i = 0; i < capacity; i++)
        {
            if (hashArray[i].status == 2)
                std::cout << i << " " << hashArray[i].value << std::endl;
        }
    }
};
