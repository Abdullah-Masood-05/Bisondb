#include <iostream>
#include <fstream>
#include <stack>
bool isJsonSyntaxValid(const std::string& file)
{
    std::stack<char> brackets;
    char prevChar = '\0'; // Track the previous character for comma and quote checks

    int i = 0;
    while (file[i] != '\0')
    {
        if (file[i] == '{' || file[i] == '[' || file[i] == '(')
        {
            brackets.push(file[i]);
        }
        else if (file[i] == '}' || file[i] == ']' || file[i] == ')')
        {
            if (brackets.empty() ||
                (file[i] == '}' && brackets.top() != '{') ||
                (file[i] == ']' && brackets.top() != '[') ||
                (file[i] == ')' && brackets.top() != '('))
            {
                std::cerr << "JSON syntax error: unexpected " << file[i] << " at position " << i << std::endl;
                return false;
            }
            brackets.pop();
        }
        else if (file[i] == ',' && prevChar != '\\')
        {
            // Check for commas, ignoring escaped commas
            if (brackets.empty() ||
                (brackets.top() != '{' && brackets.top() != '['))
            {
                std::cerr << "JSON syntax error: unexpected comma at position " << i << std::endl;
                return false;
            }
        }
        else if (file[i] == '"' && prevChar != '\\')
        {
            // Check for quotes, ignoring escaped quotes
            brackets.push(file[i]);
            i++; // Move to the next character after the opening quote
            while (file[i] && file[i] != '"')
            {
                if (file[i] == '\\' && prevChar != '\\')
                {
                    // Skip escaped characters
                    i++;
                }
                i++;
            }
            if (!file[i])
            {
                std::cerr << "JSON syntax error: unmatched quote starting at position " << i << std::endl;
                return false;
            }
            brackets.pop(); // Pop the opening quote
        }

        prevChar = file[i];
        i++;
    }

    if (!brackets.empty())
    {
        std::cerr << "JSON syntax error: unmatched " << brackets.top() << std::endl;
        return false;
    }

    return true;
}