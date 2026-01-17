# Базовый образ ROS2 Jazzy
ARG ROS_DISTRO=jazzy
FROM microros/micro-ros-agent:${ROS_DISTRO}

# Установка переменных окружения для избежания интерактивных запросов
ENV DEBIAN_FRONTEND=noninteractive
# Use RMW Micro XRCE-DDS implementation
ENV RMW_IMPLEMENTATION=rmw_microxrcedds

# Настройка окружения ROS2 в bashrc
RUN echo "source /opt/ros/${ROS_DISTRO}/setup.bash" >> /root/.bashrc

# Создание рабочей директории
WORKDIR /ros2_ws

