#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>
#include<fstream>
#include <time.h>
#include<iostream> 
#include <iostream>
#include <winsock2.h>
#include <WS2tcpip.h>
#include <string>
#include <fstream>
#include "json_check.h"
#include <limits>
#pragma comment(lib, "Ws2_32.lib")
using namespace std;
int levels(sf::RenderWindow& window, sf::Texture& insert, sf::Texture& del, sf::Texture& find, sf::Texture& find_val) {

    sf::Sprite e(insert), m(del), h(find), i(find_val);
    int currentOption = 0;
    bool eval = false;

    int value_return = -1;

    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event))
        {
            if (event.type == sf::Event::Closed) {
                window.close();

            }

            if (event.type == sf::Event::KeyPressed) {
                if (event.key.code == sf::Keyboard::Down) {
                    currentOption = (currentOption + 1) % 4;
                }
                else if (event.key.code == sf::Keyboard::Up) {
                    currentOption = (currentOption + 3) % 4;
                }
                else if (event.key.code == sf::Keyboard::Return) {
                    if (currentOption == 0) 
                    {
                        window.close();
                        return 1;
                    }
                    else if (currentOption == 1) 
                    {
                        window.close();
                        return 3;
                    }
                    else if (currentOption == 2)
                    {
                        window.close();
                        return 2;
                    }
                    else if (currentOption == 3) 
                    {
                        window.close();
                        return 4;
                    }
                }
            }
        }
        window.clear();
        // Draw the current selected option
        if (currentOption == 0)
            window.draw(e);
        else if (currentOption == 2)
            window.draw(m);
        else if (currentOption == 1)
            window.draw(h);
        else if (currentOption == 3)
            window.draw(i);

        window.display();
        if (eval == true) {
            break;
        }
    }
    return -1;
}


int main()
{


    WSADATA wsData;
    if (WSAStartup(MAKEWORD(2, 2), &wsData) != 0)
    {
        cerr << "Failed to initialize Winsock" << std::endl;
        return 1;
    }

    // Connect to the server
    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == INVALID_SOCKET)
    {
        cerr << "Failed to create socket" << std::endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &(serverAddr.sin_addr));
    serverAddr.sin_port = htons(12345);

    if (connect(clientSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        cerr << "Failed to connect to the server" << std::endl;
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }
    do
    {
        sf::RenderWindow window(sf::VideoMode(800, 500), "Documnet Manager");
        sf::Texture  Insert, Delete, Find, FIndbyValue;


        Insert.loadFromFile("C:/Users/DELL/Downloads/TETRoMANIA/im/insert.png");
        Delete.loadFromFile("C:/Users/DELL/Downloads/TETRoMANIA/im/delete.png");
        Find.loadFromFile("C:/Users/DELL/Downloads/TETRoMANIA/im/find.png");
        FIndbyValue.loadFromFile("C:/Users/DELL/Downloads/TETRoMANIA/im/value.png");
        int value = levels(window, Insert, Delete, Find, FIndbyValue);

        char a = 1;
        char arr[256];
        string input;
        if (value == 1)
        {
            input = "db.insertOne";
            send(clientSocket, input.c_str(), input.size() + 1, 0);
            cout << "Enter data to input!\n";
            string data;
            cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            getline(std::cin, data);
            send(clientSocket, data.c_str(), data.size() + 1, 0);
            // Validate JSON syntax
            if (!isJsonSyntaxValid(data))
            {
                cerr << "Invalid JSON syntax!\n";
                continue;
            }
        }
        else if (value == 3)
        {
            input = "db.find_";
            send(clientSocket, input.c_str(), input.size() + 1, 0);
            while (a != 0)
            {
                recv(clientSocket, &a, sizeof(char), 0);
                cout << a;
            }
        }
        else if (value == 4)
        {
            a = 0;
            input = "db.find";
            send(clientSocket, input.c_str(), input.size() + 1, 0);
            string field, value;
            cin >> field;
            send(clientSocket, field.c_str(), field.size() + 1, 0);
            getline(std::cin, value);
            send(clientSocket, value.c_str(), value.size() + 1, 0);
            while (a == 0)
            {
                recv(clientSocket, &a, sizeof(char), 0);
                if (a == 1)
                    break;
                recv(clientSocket, arr, sizeof(arr), 0);
                cout << arr << std::endl;
            }
        }
        else if (value == 2)
        {
            input = "db.delete";
            send(clientSocket, input.c_str(), input.size() + 1, 0);
            string field, value;
            cout << "Enter field and value to delete records (e.g., db.delete cgpa 3.8): ";
            cin >> field >> value;
            send(clientSocket, field.c_str(), field.size() + 1, 0);
            send(clientSocket, value.c_str(), value.size() + 1, 0);
            recv(clientSocket, arr, sizeof(arr), 0);
            cout << arr << std::endl;
        }
    } while (true);
    closesocket(clientSocket);
    WSACleanup();


    return 0;
}
