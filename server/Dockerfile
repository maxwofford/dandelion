FROM oven/bun:1-alpine

RUN apk add --no-cache git \
 && git config --system http.lowSpeedLimit 1000 \
 && git config --system http.lowSpeedTime 30 \
 && git config --system clone.timeout 120

WORKDIR /app
COPY index.js .

ENV PORT=3000
EXPOSE 3000

CMD ["bun", "run", "index.js"]
