# ROS2 Jazzy Docker Setup

Базовая конфигурация Docker для запуска ROS2 Jazzy Jalisco.

## Требования

- Docker
- Docker Compose

## Использование

### Сборка образа

```bash
docker-compose build
```

### Запуск контейнера

```bash
docker-compose up -d
```

### Подключение к контейнеру

```bash
docker-compose exec ros2-jazzy bash
```

Или используя docker напрямую:

```bash
docker exec -it ros2-jazzy-container bash
```

### Остановка контейнера

```bash
docker-compose down
```

## Структура

- `Dockerfile` - определение образа ROS2 Jazzy
- `docker-compose.yml` - конфигурация для запуска контейнера
- `ros2_ws/` - рабочее пространство ROS2 (создается автоматически)

## Примечания

- Рабочее пространство ROS2 монтируется в директорию `./ros2_ws`
- Для поддержки GUI приложений раскомментируйте соответствующие строки в `docker-compose.yml`
- ROS2 Jazzy работает на Ubuntu 22.04 (Jammy Jellyfish)
