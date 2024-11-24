FROM gcc:latest

RUN apt-get update && apt-get install -y iproute2

WORKDIR /app

COPY master.cpp common.h ./

RUN g++ -o master master.cpp

EXPOSE 5000/udp

CMD ["./master"]
