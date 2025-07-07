FROM ubuntu:latest

RUN apt-get update
RUN apt-get install -y build-essential
RUN apt-get install -y python3
RUN apt-get install -y make
RUN apt-get install -y clang

WORKDIR /app

COPY . .

RUN make test

CMD ["bash", "-c", "cd build && ./test & sleep 1 && python3 test.py"]
