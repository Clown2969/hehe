# SimpleFS

## Запуск (Docker + Web)
```bash
docker build -t simplefs .
docker run --rm -it -p 7681:7681 simplefs
```
Открыть в браузере: [http://localhost:7681](http://localhost:7681)

## Доступные команды (в консоли /mnt)
- `test demo /mnt` — полный тест записи/чтения
- `test meta /mnt` — список хэшей файлов
- `test map /mnt file001` — маппинг секторов файла
- `test zero /mnt` — обнуление всех файлов
- `test erase /mnt` — стирание суперблоков ФС
- `echo "data" > /mnt/file001` — ручная запись
- `cat /mnt/file001` — ручное чтение


