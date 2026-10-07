# 0.4.9 — прицел, мини-карта, плавный поворот, тело за камерой

Запрос пользователя (2026-10-07): вернуть прицел и мини-карту, плавный поворот камеры, персонаж поворачивается за камерой.

- Прицел: луч вдоль ствола (grip -Y, тот же, что для выстрела с 0.4.8), shape test от правой руки, крестик DRAW_RECT
  в точке попадания (красный, если в точке человек/машина/объект). Обычный прицел GTA (HUD 14) скрывается, т.к. он
  показывает, куда смотрит голова. Рисуется только с огнестрелом/метательным. ini: crosshair=1, crosshair_size=6.
- Мини-карта: радар сидит в левом нижнем углу кадра 16:9, а квадратная обрезка для шлема его отрезала. Шейдер blit
  теперь копирует этот угол в видимую часть изображения для глаза. ini: minimap=1, minimap_x=20, minimap_y=66,
  minimap_size=22 (% картинки в шлеме), minimap_src_w=19, minimap_src_h=25 (% кадра 16:9). Если радар скрыт, включается DISPLAY_RADAR.
- Плавный поворот правым стиком (по умолчанию): turn_mode=smooth|snap, turn_speed=120 °/с, snap_angle=30.
- Тело за камерой (пешком, стоя): если взгляд ушёл больше чем на body_deadzone=35° (10° при прицеливании/стрельбе),
  тело доворачивается со скоростью body_speed=240 °/с через SET_ENTITY_HEADING. Не работает при ходьбе (GTA поворачивает
  сама), в машине, в ragdoll и при посадке. body_follow=0 отключает.
- Новые натив-функции: START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE, GET_SHAPE_TEST_RESULT, GET_SCREEN_COORD_FROM_WORLD_COORD,
  DRAW_RECT, IS_PED_ARMED, GET_ASPECT_RATIO, HIDE_HUD_COMPONENT_THIS_FRAME, SET_ENTITY_HEADING, IS_PED_RAGDOLL,
  IS_PED_GETTING_INTO_A_VEHICLE, GET_FRAME_TIME, GET_SAFE_ZONE_SIZE, IS_RADAR_HIDDEN, DISPLAY_RADAR.

В игре не проверено. Положение мини-карты зависит от настроек экрана/безопасной зоны и может потребовать подстройки minimap_*.
