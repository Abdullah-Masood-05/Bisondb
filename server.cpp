#include <iostream>
#include <winsock2.h>
#include <fstream>
#include <string>
#include <ctime>
#include "helper.h"
#include <cstring>
#include "hash_map.h"
#include <string>
using namespace std;
#pragma comment(lib, "ws2_32.lib")
#define Max_size 1024

string get_current_time()
{
    time_t curr_time;
    struct tm time_info;
    char Cap[80];

    time(&curr_time);
    localtime_s(&time_info, &curr_time);
    strftime(Cap, sizeof(Cap), "%Y-%m-%d %H:%M:%S", &time_info);
    return string(Cap);
}

DWORD WINAPI HandleClient(LPVOID lpParam)
{
    SOCKET client_socket = (SOCKET)lpParam;
    char c;
    hash_map<string, string> map;

    char user_command[256];
    do
    {
        /// recib=ving the request
        if (recv(client_socket, user_command, sizeof(user_command), 0) == SOCKET_ERROR)
        {
            cerr << "Failed to command from client.....(:/)\n";
            closesocket(client_socket);
            return 1;
        }
        string data = user_command;
        memset(user_command, 0, sizeof(user_command));
        if (data == "db.insertOne")
        {
            recv(client_socket, user_command, sizeof(user_command), 0);
            data = user_command;
            string objectId = ObjectIdGenerator::generateObjectId();
            // Append the data with Object ID to the file
            ofstream outFile("data.txt", ios::app);
            if (!outFile.is_open())
            {
                cerr << "Error opening file!\n";
                return 1;
            }
            outFile << "{ \"_id\": \"" << objectId << "\", " << data.substr(1) << endl; // Append Object ID
            cout << "Data inserted!\n";
        }
        else if (data == "db.find_")
        {
            ifstream file("data.txt");
            file.clear();
            file.seekg(0, ios::beg);
            while (file.get(c))
            {
                send(client_socket, &c, sizeof(c), 0);
                cout << c;
            }
            c = 0;
            send(client_socket, &c, sizeof(c), 0);
            cout << endl;
        }
        else if (data == "db.find")
        {
            ifstream file("data.txt");
            file.clear();
            file.seekg(0, ios::beg);
            memset(user_command, 0, sizeof(user_command));
            recv(client_socket, user_command, sizeof(user_command), 0);
            string field = user_command;
            memset(user_command, 0, sizeof(user_command));
            recv(client_socket, user_command, sizeof(user_command), 0);
            char gg = 0;
            string value = user_command;
            while (getline(file, map["data"]))
            {

                int found = map["data"].find("\"" + field + "\":");
                if (found != string::npos)
                {
                    int start = found + field.length() + 4;
                    int end = map["data"].find(",", start);
                    string fieldValue = map["data"].substr(start, end - start);

                    if (fieldValue.substr(0, 3) == value.substr(1, value.length()))
                    {
                        send(client_socket, &gg, sizeof(gg), 0);
                        send(client_socket, map["data"].c_str(), map["data"].size() + 1, 0);
                        cout << map["data"] << endl;
                    }
                    int d = fieldValue.length();
                    // cout << fieldValue.substr(0, 3) << endl;
                    if (fieldValue.substr(1, d - 2) == value.substr(1, value.length()))
                    {
                        send(client_socket, &gg, sizeof(gg), 0);
                        send(client_socket, map["data"].c_str(), map["data"].size() + 1, 0);
                        cout << map["data"] << endl;
                    }
                }
            }
            gg = 1;
            send(client_socket, &gg, sizeof(gg), 0);
        }
        else if (data == "db.delete")
        {
            memset(user_command, 0, sizeof(user_command));
            recv(client_socket, user_command, sizeof(user_command), 0);
            string field = user_command;
            memset(user_command, 0, sizeof(user_command));
            recv(client_socket, user_command, sizeof(user_command), 0);
            char gg = 0;
            string value = user_command;
            if (deleteRecord("data.txt", field, value))
            {
                cout << "Records deleted!\n";
                string deleted = "Records deleted!\n";
                send(client_socket, deleted.c_str(), deleted.size() + 1, 0);
                ifstream temp("temp.txt");
                ofstream outFile("data.txt", ios::trunc);
                temp.clear();
                temp.seekg(0, ios::beg);
                while (temp.get(c))
                {
                    outFile << c;
                }
                outFile << endl;
            }
            else
            {
                send(client_socket, &gg, sizeof(gg), 0);
                string error = "Error deleting records!\n";
                send(client_socket, error.c_str(), error.size() + 1, 0);
                cerr << "Error deleting records!\n";
            }
            gg = 1;
            send(client_socket, &gg, sizeof(gg), 0);
        }
        else if (data == "1")
        {
            cout << "Client disconnected\n";
            break;
        }
    } while (true);

    closesocket(client_socket);
    return 0;
}

int main()
{
    WSADATA wsaData;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        cerr << "Failed to initialize Winsock........(:/)\n";
        return 1;
    }

    SOCKET Server_Side = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in Server_add;

    if (Server_Side == INVALID_SOCKET)
    {
        cerr << "Failed to create socket........(:/)\n";
        WSACleanup();
        return 1;
    }

    Server_add.sin_family = AF_INET;
    Server_add.sin_addr.s_addr = INADDR_ANY;
    Server_add.sin_port = htons(12345);

    if (bind(Server_Side, (struct sockaddr *)&Server_add, sizeof(Server_add)) == SOCKET_ERROR)
    {
        cerr << "Binding failed........(:/)\n";
        closesocket(Server_Side);
        WSACleanup();
        return 1;
    }

    if (listen(Server_Side, SOMAXCONN) == SOCKET_ERROR)
    {
        cerr << "Listing failed..........Nothing Found (:/)\n";
        closesocket(Server_Side);
        WSACleanup();
        return 1;
    }

    cout << "Server is listening on port " << 12345 << endl;

    while (true)
    {
        SOCKET Cleint_socket = accept(Server_Side, NULL, NULL);
        if (Cleint_socket == INVALID_SOCKET)
        {
            cerr << "Accept failed" << endl;
            closesocket(Server_Side);
            WSACleanup();
            return 1;
        }

        DWORD ID_thread;
        HANDLE thread_handle = CreateThread(NULL, 0, HandleClient, (LPVOID)Cleint_socket, 0, &ID_thread);
        if (thread_handle == NULL)
        {
            cerr << "Failed to create thread for client.......(:/)";
            closesocket(Cleint_socket);
        }
        else
        {
            CloseHandle(thread_handle);
        }
    }

    closesocket(Server_Side);
    WSACleanup();

    return 0;
}
