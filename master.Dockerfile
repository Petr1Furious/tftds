FROM gcc:latest

WORKDIR /app

COPY master.cpp common.h ./

RUN apt-get update && apt-get install -y iproute2 \
    && g++ -o master master.cpp

EXPOSE 5000/udp

CMD ["./master"]
