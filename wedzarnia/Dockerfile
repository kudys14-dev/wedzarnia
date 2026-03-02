# ============================================================
# Dockerfile — Wędzarnia IoT PWA
# Zmienne VITE_* są wbudowywane w czasie budowania (build-time)
# ============================================================

# ---- Etap 1: budowanie aplikacji ----
FROM node:20-alpine AS builder

WORKDIR /app

# Argumenty budowania — przekazywane przez docker-compose.yml
ARG VITE_SUPABASE_URL
ARG VITE_SUPABASE_PUBLISHABLE_KEY
ARG VITE_SUPABASE_PROJECT_ID

# Vite wymaga zmiennych jako ENV żeby import.meta.env działało
ENV VITE_SUPABASE_URL=$VITE_SUPABASE_URL
ENV VITE_SUPABASE_PUBLISHABLE_KEY=$VITE_SUPABASE_PUBLISHABLE_KEY
ENV VITE_SUPABASE_PROJECT_ID=$VITE_SUPABASE_PROJECT_ID

# Zainstaluj zależności (osobna warstwa — cache jeśli package.json się nie zmienił)
COPY package*.json ./
RUN npm ci

# Skopiuj kod i zbuduj
COPY . .
RUN npm run build

# ---- Etap 2: serwowanie przez nginx ----
FROM nginx:alpine AS production

# Skopiuj zbudowaną aplikację
COPY --from=builder /app/dist /usr/share/nginx/html

# Konfiguracja nginx
COPY nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 80

CMD ["nginx", "-g", "daemon off;"]
