#include <initializer_list>
#include<iostream>
template <class T>
class vector
{
protected:
    T *arr;
    int capacity;

public:
    int _size;

    vector()
    {
        this->_size = 0;
        this->capacity = 1;
        this->arr = new T[capacity];
        arr[0] = 0;
    }
    vector(int size)
    {
        this->_size = size;
        this->capacity = size;
        this->arr = new T[capacity]();
    }
    vector(std::initializer_list<T> initList)
    {
        this->capacity = initList.size();
        this->_size = initList.size();
        this->arr = new T[capacity];

        int i = 0;
        for (const T &element : initList)
        {
            this->arr[i++] = element;
        }
    }
    int size()
    {
        return _size;
    }

    void push_back(T data)
    {
        if (this->_size >= this->capacity)
        {
            T *new_arr = new T[this->capacity * 2];
            for (int i = 0; i < this->_size; i++)
            {
                new_arr[i] = this->arr[i];
            }
            new_arr[this->_size] = data;
            delete[] this->arr;
            this->arr = new_arr;
            this->_size++;
            this->capacity = this->capacity * 2;
        }
        else
        {
            this->arr[this->_size] = data;
            this->_size++;
        }
    }
    void pop_back()
    {
        if (_size != 0)
        {
            T *new_arr = new T[_size - 1];
            for (int i = _size - 2, j = 0; i >= 0; i--, j++)
                new_arr[j] = arr[j];
            delete[] arr;
            arr = new_arr;
            this->_size--;
            this->capacity = this->_size;
        }
        else
            std::cout << "\n Out of range!\n";
    }

    T *begin()
    {
        return arr;
    }

    T *end()
    {
        return arr + _size;
    }

    T &operator[](int index)
    {
        return arr[index];
    }

    void print()
    {
        for (int i = 0; i < _size; i++)
            std::cout << arr[i] << " ";
    }
    void resize(int newCapacity)
    {
        T *new_arr = new T[newCapacity];
        int copySize = std::min(newCapacity, this->_size);

        for (int i = 0; i < copySize; i++)
        {
            new_arr[i] = this->arr[i];
        }

        delete[] this->arr;
        this->arr = new_arr;
        this->capacity = newCapacity;
    }

    ~vector()
    {
        delete[] arr;
    }
};
template <>
class vector<bool>
{
private:
    unsigned char *arr;
    int capacity;

public:
    int _size;
    vector()
    {
        this->_size = 0;
        this->capacity = 1;
        this->arr = new unsigned char[capacity]();
    }

    vector(int size, bool value)
    {
        this->_size = size;
        this->capacity = (size + 7) / 8; // Calculate required capacity in bytes
        this->arr = new unsigned char[capacity]();

        // Set all bits to the specified boolean value
        unsigned char boolValue = value ? 0xFF : 0x00;
        std::fill_n(arr, capacity, boolValue);
    }

    bool operator[](int index) const
    {
        return (arr[index / 8] >> (index % 8)) & 1;
    }
    void print()
    {
        for (int i = 0; i < _size; i++)
            std::cout << arr[i] << " ";
    }
    ~vector()
    {
        delete[] arr;
    }
};
