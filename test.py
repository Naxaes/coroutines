import random
import threading
import socket
import time
from string import printable


HEADER = """HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 119\r\nConnection: close\r\n\r\n<!DOCTYPE html>\n<html lang="en">\n<head>\n  <meta charset="UTF-8">\n  <title>Title</title>\n</head>\n<body>\n\n</body>\n</html>"""

def client_thread(host, port, i):
    try:
        duration = random.randint(1, 20)
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        j = 0
        end_time = time.time() + duration
        while time.time() < end_time:
            message = f'Hello from {i} time {j} ' + ''.join(random.choice(printable) for _ in range(random.randint(1, 512)))
            j += 1
            sock.sendall(message.encode())
            answer = sock.recv(1024).decode('utf-8')

            assert answer == HEADER + message, f'"{repr(answer)}" != "{repr(HEADER + message)}"'
            time.sleep(random.random())
        sock.close()
    except Exception as e:
        print(e)

def stress_test(host, port, num_clients):
    threads = []
    for i in range(num_clients):
        thread = threading.Thread(target=client_thread, args=(host, port, i))
        threads.append(thread)
        thread.start()
    for thread in threads:
        thread.join()


stress_test(host = '127.0.0.1', port = 6969, num_clients = 1000)



