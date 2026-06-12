#include <iostream>
#include <fstream>
#include <cstdint>

// Helper function to read an int64 from binary data
int64_t readInt64(std::ifstream &stream)
{
    int64_t value = 0;
    stream.read(reinterpret_cast<char *>(&value), sizeof(value));
    return value;
}

// Helper function to add leading zeros to a number when converting to string
std::string leadZero(int num)
{
    return (num < 10 ? "0" : "") + std::to_string(num);
}

// Function to convert Unix time in milliseconds to a formatted date string
std::string unixTimeToDate(int64_t unixTime)
{
    // Constants for time conversion
    const int64_t kMillisecondsPerSecond = 1000;
    const int64_t kSecondsPerMinute = 60;
    const int64_t kMinutesPerHour = 60;
    const int64_t kHoursPerDay = 24;
    const int64_t kDaysPerYear = 365;
    const int kDaysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; // Replace std::array with a regular array

    // Convert milliseconds to seconds and compute date components
    int64_t seconds = unixTime / kMillisecondsPerSecond;
    int64_t days = seconds / (kSecondsPerMinute * kMinutesPerHour * kHoursPerDay);
    seconds %= kSecondsPerMinute * kMinutesPerHour * kHoursPerDay;
    int64_t hours = seconds / (kSecondsPerMinute * kMinutesPerHour);
    seconds %= kSecondsPerMinute * kMinutesPerHour;
    int64_t minutes = seconds / kSecondsPerMinute;
    seconds %= kSecondsPerMinute;

    // Calculate year
    int64_t year = 1970;
    while (days > kDaysPerYear)
    {
        if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))
        {
            if (days > kDaysPerYear + 1)
            {
                days -= (kDaysPerYear + 1);
                year += 1;
            }
        }
        else
        {
            days -= kDaysPerYear;
            year += 1;
        }
    }

    // Calculate month and day
    int month = 0;
    for (int i = 0; i < 12; i++)
    {
        int monthDays = kDaysPerMonth[i];
        if (i == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)))
        {
            monthDays += 1; // February in a leap year
        }
        if (days > monthDays)
        {
            days -= monthDays;
            month += 1;
        }
        else
        {
            break;
        }
    }
    int day = static_cast<int>(days) + 1;

    // Format the date string
    std::string dateStr = std::to_string(year) + "-" +
                          leadZero(month + 1) + "-" +
                          leadZero(day) + "T" +
                          leadZero(static_cast<int>(hours)) + ":" +
                          leadZero(static_cast<int>(minutes)) + ":" +
                          leadZero(static_cast<int>(seconds)) +
                          ".000Z";
    return dateStr;
}

void mian(std::ifstream &input, std::ofstream &output, int &see)
{
    // Open the BSON file
    if (!input)
    {
        std::cerr << "Cannot open file." << std::endl;
        return;
    }
    char e;
    std::string index;
    while (true)
    {
        input.read(&e, 1);
        if (e == 0)
            break;
        else if ((e > 64 && e < 91) || (e > 96 && e < 123) || (e > 47 && e < 58) || (e == 95))
        {
            index += e;
        }
        see++;
    }
    int64_t date = readInt64(input);
    if (input.fail())
    {
        std::cerr << "Failed to read date." << std::endl;
        return;
    }

    // Convert the Unix time to a date string
    std::string dateString = unixTimeToDate(date);
    std::cout << '"' << index << '"' << ": " << '"' << dateString << '"' << ", ";
    output << '"' << index << '"' << ": " << '"' << dateString << '"' << ", ";
    see += 8;
}
