# syntax=docker/dockerfile:1

FROM golang:1.25-alpine AS build

WORKDIR /src

COPY go.mod go.sum ./
RUN go mod download

COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /out/medgenet-server .

FROM alpine:3

RUN addgroup -S medgenet \
    && adduser -S -G medgenet medgenet \
    && mkdir -p /data/ghosts \
    && chown -R medgenet:medgenet /data

COPY --from=build /out/medgenet-server /usr/local/bin/medgenet-server

ENV MEDGENET_BIND_HOST=0.0.0.0 \
    MEDGENET_FESL_PORT=18680 \
    MEDGENET_HTTP_PORT=8080 \
    MEDGENET_DB_PATH=/data/me_server.db \
    MEDGENET_GHOST_DIR=/data/ghosts \
    MEDGENET_LOG_PATH=/data/session.log

EXPOSE 18680 8080
VOLUME ["/data"]

USER medgenet
ENTRYPOINT ["medgenet-server"]
