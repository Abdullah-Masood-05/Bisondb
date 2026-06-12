#include <iostream>
#include <winsock2.h>
#include <WS2tcpip.h>
#include <string>
#include <fstream>
#include "json_check.h"
#include <limits>
#pragma comment(lib, "Ws2_32.lib")
using namespace std;
int main()
{
    WSADATA wsData;
    if (WSAStartup(MAKEWORD(2, 2), &wsData) != 0)
    {
        cerr << "Failed to initialize Winsock" << endl;
        return 1;
    }

    // Connect to the server
    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == INVALID_SOCKET)
    {
        cerr << "Failed to create socket" << endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &(serverAddr.sin_addr));
    serverAddr.sin_port = htons(12345);

    if (connect(clientSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        cerr << "Failed to connect to the server" << endl;
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }
    char a = 1;
    char arr[256];

    do
    {
        cout << "Enter commands to input data!\n";
        cout << "Or press 1 to exit\n";
        string input;
        cin >> input;

        if (input == "db.insertOne")
        {
            send(clientSocket, input.c_str(), input.size() + 1, 0);
            cout << "Enter data to input!\n";
            string data;
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            getline(cin, data);
            send(clientSocket, data.c_str(), data.size() + 1, 0);
            // Validate JSON syntax
            if (!isJsonSyntaxValid(data))
            {
                cerr << "Invalid JSON syntax!\n";
                continue;
            }
        }
        else if (input == "db.find_")
        {
            send(clientSocket, input.c_str(), input.size() + 1, 0);
            while (a != 0)
            {
                recv(clientSocket, &a, sizeof(char), 0);
                cout << a;
            }
        }
        else if (input == "db.find")
        {
            a = 0;
            send(clientSocket, input.c_str(), input.size() + 1, 0);
            string field, value;
            cin >> field;
            send(clientSocket, field.c_str(), field.size() + 1, 0);
            getline(cin, value);
            send(clientSocket, value.c_str(), value.size() + 1, 0);
            while (a == 0)
            {
                recv(clientSocket, &a, sizeof(char), 0);
                if (a == 1)
                    break;
                recv(clientSocket, arr, sizeof(arr), 0);
                cout << arr << endl;
            }
        }
        else if (input == "db.delete")
        {
            send(clientSocket, input.c_str(), input.size() + 1, 0);
            string field, value;
            cout << "Enter field and value to delete records (e.g., db.delete cgpa 3.8): ";
            cin >> field >> value;
            send(clientSocket, field.c_str(), field.size() + 1, 0);
            send(clientSocket, value.c_str(), value.size() + 1, 0);
            recv(clientSocket, arr, sizeof(arr), 0);
            cout << arr << endl;
        }
        else if (input == "1")
        {
            send(clientSocket, input.c_str(), input.size() + 1, 0);
            system("pause");
            break;
        }
    } while (true);

    closesocket(clientSocket);
    WSACleanup();
    return 0;
}
