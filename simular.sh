#!/bin/bash
# Simula el NodeMCU: envía temperatura, humedad, gas y llama cada 5 s, 30 veces.
# Sirve para probar el stack sin el dispositivo.
# Uso: ./simular.sh            (API en localhost)
#      API=http://<ip>:8000 ./simular.sh

API="${API:-http://localhost:8000}"
BASE='"sensor":"dc-rack-01","cliente":"cliente-007","ubicacion":"Sala servidores - Rack 1"'

enviar() {
  curl -s -o /dev/null -w "$1=$2 -> HTTP %{http_code}   " -X POST "$API/api/v1/lecturas" \
    -H "Content-Type: application/json" \
    -d "{$BASE,\"tipo\":\"$1\",\"valor\":$2,\"unidad\":\"$3\"}"
}

for i in $(seq 1 30); do
  T=$(awk -v s=$RANDOM 'BEGIN { srand(s); printf "%.1f", 22 + rand() * 10 }')   # 22 a 32 °C
  H=$(awk -v s=$RANDOM 'BEGIN { srand(s); printf "%.0f", 45 + rand() * 30 }')   # 45 a 75 %
  G=$(awk -v s=$RANDOM 'BEGIN { srand(s); printf "%.0f", 150 + rand() * 150 }') # 150 a 300
  L=0
  # Entre la lectura 20 y 23 se simula un incendio: sube el gas y aparece llama
  if [ "$i" -ge 20 ] && [ "$i" -le 23 ]; then G=$((600 + RANDOM % 200)); L=1; fi

  enviar temperatura "$T" C
  enviar humedad "$H" %
  enviar gas "$G" raw
  enviar llama "$L" bool
  echo
  sleep 5
done
