#include <cstring>
#include <sstream>
#include <iomanip>
#include "helper.h"
#include "vector.h"
#include "new.h"
void decoder(std::ifstream &input, std::ofstream &output);
void decodeBsonArray(std::ifstream &input, std::ofstream &output, bool checks, int &see, int number)
{
    char asags;
    asags = input.peek();
    if (see == number)
    {
        return;
    }
    char elementType;
    bool check = false;
    input.read(&elementType, 1);
    see++;
    if (see == number)
    {
        std::cout << "}\n";
        output << "  }\n";
        return;
    }
    if (elementType == 0)
    {
        if (elementType == 0)
        {
            std::cout << "}";
            output << "}";
            char d;
            d = input.peek();
            while (d != '\r' && d == 0)
            {
                d = input.peek();
                if (d == '\r' || d == 23 || d == -76 || d == -75)
                {
                    d = input.peek();
                    return;
                }
                else if (d == 0)
                {
                    input.read(&d, 1);
                    see++;
                    continue;
                }
                else
                {
                    std::cout << ", ";
                    output << ", ";
                    decodeBsonArray(input, output, check, see, number);
                    return;
                }
            }
            if (d == '\r' || d == 23)
            {
                input.read(&d, 1);
                d = input.peek();
                return;
            }
            else if (d == 0)
            {
                std::cout << "},\n";
                output << "  },\n";
            }
            else
            {
                std::cout << ", ";
                output << ", ";
                decodeBsonArray(input, output, check, see, number);
                return;
            }
        }
    }
    int d = elementType;

    while (elementType != 0)
    {
        // String
        if (elementType == 2)
        {
            string_h(input, output, checks, see);
            decodeBsonArray(input, output, checks, see, number);
            return;
        }
        else if (elementType == 1)
        { // Double
            std::string index;
            int i = 6;
            char e;
            while (true)
            {
                input.read(&e, 1);
                see++;
                char bb = true;
                if (e == 0)
                    break;
                else if ((e > 64 && e < 91) || (e > 96 && e < 123) || (e == 95))
                {
                    index += e;
                }
                if (bb = false)
                {
                    if ((e > 47 && e < 58))
                        index += e;
                }
                else
                {
                    bb = false;
                    continue;
                }
            }
            double num;
            input.read((char *)&num, sizeof(double));
            see += 8;
            if (!(index.length() >= 1))
            {
                std::cout << num;
                output << num;
            }
            else
            {
                std::cout << '"' << index << '"' << " : ";
                output << '"' << index << '"' << " : ";
                std::cout << num;
                output << num;
            }
            char a;
            a = input.peek();
            if (a > 0)
            {
                std::cout << ", ";
                output << ", ";
            }
            else if (a == 0 && checks == true)
            {
                std::cout << ']';
                output << ']';
                input.read(&a, 1);
                see++;
                checks = false;
            }

            decodeBsonArray(input, output, checks, see, number);
            return;
        }

        else if (elementType == 16)
        { // Int32
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
            int32_t num;
            input.read((char *)&num, sizeof(int32_t));
            see += 4;
            if (!(index.length() >= 2))
            {
                std::cout << num;
                output << num;
            }
            else
            {
                std::cout << '"' << index << '"' << " : ";
                output << '"' << index << '"' << " : ";
                std::cout << num;
                output << num;
            }
            char a;
            a = input.peek();
            if (a > 0)
            {
                std::cout << ", ";
                output << ", ";
            }
            else if (a == 0 && checks == true)
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
                    checks = false;
                }
            }

            decodeBsonArray(input, output, checks, see, number);
            return;
        }
        else if (elementType == 3)
        {
            // Object
            object(input, output, see);
            decodeBsonArray(input, output, check, see, number);
            return;
        }
        else if (elementType == 4)
        { // Array
            arr(input, output, see);
            bool h = true;
            decodeBsonArray(input, output, h, see, number);

            return;
        }
        else if (elementType == 7)
        {
            // Object_Id
            std::string name;
            int32_t num;
            input.read((char *)&num, sizeof(int32_t));
            see += 4;
            char e;
            vector<uint8_t> b;
            for (int it = 0; it < 12; it++)
            {
                input.read(&e, 1);
                see++;
                b.push_back(e);
            }
            // int number = std::stoi(name);
            std::cout << "{";
            std::cout << '"' << "_id" << '"';
            std::cout << " : " << '"';
            std::stringstream hexStream;
            hexStream << std::hex << std::setfill('0');
            for (int it = 0; it < 12; it++)
            {
                hexStream << std::setw(2) << static_cast<int>(b[it]);
            }
            std::string objectId = hexStream.str();
            std::cout << '"' << objectId << '"';
            output << "{";
            output << '"' << "_id" << '"';
            output << ":";
            output << '"' << objectId << '"';
            char a;
            a = input.peek();
            if (a > 0)
            {
                std::cout << ", ";
                output << ", ";
            }
            else
            {
                std::cout << ']';
                output << ']';
            }
            bool h = true;
            decodeBsonArray(input, output, h, see, number);
            return;
        }
        else if (elementType == 18)
        {
            // 64 bit integer
            char e;
            while (true)
            {
                input.read(&e, 1);
                see++;
                if (e == 0)
                    break;
            }
            see += 8;
            int64_t num;
            input.read((char *)&num, sizeof(int64_t));
            std::cout << num;
            output << num;
            char a;
            a = input.peek();
            if (a > 0)
            {
                std::cout << ", ";
                output << ", ";
            }
            else if (a == 0 && checks == true)
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
                    checks = false;
                }
            }

            decodeBsonArray(input, output, check, see, num);
            return;
        }
        else if (elementType == 9)
        {
            // Date
            mian(input, output, see);
            bool h = true;
            decodeBsonArray(input, output, h, see, number);
            return;
        }
        else if (elementType == 19)
        {
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
            see += 16;
            __int128_t num;
            input.read((char *)&num, sizeof(__int128_t));
            std::string int128String = std::to_string(static_cast<long long>(num >> 64)) +
                                       std::to_string(static_cast<long long>(num & ((1ULL << 64) - 1)));
            if (!(index.length() >= 2))
            {
                std::cout << int128String;
                output << int128String;
            }
            else
            {
                std::cout << '"' << index << '"' << " : ";
                output << '"' << index << '"' << " : ";
                std::cout << int128String;
                output << int128String;
            }
            std::cout << int128String;
            output << int128String;
            char a;
            a = input.peek();
            if (a > 0)
            {
                std::cout << ", ";
                output << ", ";
            }
            else if (a == 0 && checks == true)
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
                    checks = false;
                }
            }

            decodeBsonArray(input, output, check, see, number);
            return;
        }
        else if (elementType == 8)
        {
            char e;
            while (true)
            {
                input.read(&e, 1);
                see++;
                if (e == 0)
                    break;
            }
            bool num;
            input.read((char *)&num, sizeof(bool));
            see++;
            if (num == true)
            {
                std::cout << "true";
                output << "true";
            }
            else
            {
                std::cout << "false";
                output << "false";
            }
            char a;
            a = input.peek();
            if (a > 0)
            {
                std::cout << ", ";
                output << ", ";
            }
            else if (a == 0 && checks == true)
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
                    checks = false;
                }
            }

            decodeBsonArray(input, output, check, see, number);
            return;
        }
        else
        {
            std::cerr << "Unsupported BSON element type: " << static_cast<int>(elementType) << std::endl;
            return;
        }
    }
}
void decoder(std::ifstream &input, std::ofstream &output)
{
    bool check = false;
    std::cout << "{\n";
    output << "{\n";
    while (!input.eof())
    {
        int num = 0;
        char e = input.peek();
        int j = 4;
        input.read((char *)&num, 4);
        if (num == 0)
            return;
        decodeBsonArray(input, output, check, j, num);
    }
    std::cout << "}\n";
    output << "}\n";
    return;
}

int main()
{
    std::string input_file = "zips.bson";
    std::ifstream input(input_file, std::ios::binary);
    std::ofstream output("output.json");
    decoder(input, output);
    system("pause");
    return 0;
}