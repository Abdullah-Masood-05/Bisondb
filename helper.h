#include <iostream>
#include <fstream>
void arr(std::ifstream &input, std::ofstream &output, int &see)
{
    std::cout << '"';
    output << '"';
    std::string name;
    char e;
    input.read(&e, 1);
    see++;
    while (true)
    {
        if (e == 0)
            break;
        name += e;
        input.read(&e, 1);
        see++;
    }
    std::cout << name << '"' << " : ";
    output << name << '"' << " : ";
    int num;
    input.read((char *)&num, 4);
    see += 4;
    std::cout << "[";
    output << "[";
    //see++;
}
void object(std::ifstream &input, std::ofstream &output, int &see)
{
    std::cout << '"';   
    output << '"';
    std::string name;
    char e;
    input.read(&e, 1);
    see++;
    while (true)
    {
        if (e == 0)
            break;
        name += e;
        input.read(&e, 1);
        see++;
    }
    //see++;
    std::cout << name << '"' << " : ";
    output << name << '"' << " : ";
    int num;
    input.read((char *)&num, 4);
    see += 4;
    std::cout << "{\n";
    output << "{\n";
}
void string_h(std::ifstream &input, std::ofstream &output, bool &chec, int &see)
{
    std::string name;
    std::string index;
    int i = 6;
    char e;
    while (true)
    {
        input.read(&e, 1);
        see++;
        if (e == 0)
            break;
        else if ((e > 64 && e < 91) || (e > 96 && e < 123) || (e > 47 && e < 58) || (e == 95))
        {
            index += e;
        }
        else
        {
            continue;
        }
    }
    int num;
    input.read((char *)&num, 4);
    see += 4;
    for (int i = 0; i < num - 1; i++)
    {
        input.read(&e, 1);
        name += e;
        see++;
    }
    input.read(&e, 1);
    see++;
    if (index.length() >= 2)
    {
        std::cout << '"' << index << '"' << " : ";
        output << '"' << index << '"' << " : ";
    }
    std::cout << '"' << name << '"';
    output << '"' << name << '"';
    char a;
    a = input.peek();
    if (a > 0)
    {
        std::cout << ", ";
        output << ", ";
    }
    else if (a == 0 && chec == true)
    {
        std::cout << ']';
        output << ']';
        input.read(&a, 1);
        see++;
        a = input.peek();
        if (a > 0)
        {
            std::cout << ", ";
            output << ", ";
        }
        chec = false;
    }
}
