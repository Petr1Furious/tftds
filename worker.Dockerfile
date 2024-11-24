FROM gcc:latest

RUN apt-get update && apt-get install -y iproute2

WORKDIR /app

COPY worker.cpp common.h ./

RUN g++ -o worker worker.cpp

EXPOSE 5000/udp
EXPOSE 6000

CMD ["./worker"]
