# ESP32 BLE RSSI Tabanlı İç Mekan Konum Takip & Karar Motoru Sistemi

Bu proje; Bluetooth Düşük Enerji (BLE) yayınları yapan nesne etiketlerinin (**IoT Tag**), ortamdaki ESP32 tarayıcı istasyonları (**IoT Scanner Gateway**) tarafından taranıp sinyal gücü (RSSI) değerlerinin filtrelenerek MQTT üzerinden merkezi bir sunucuya aktarılmasını ve Raspberry Pi üzerinde Docker ile koşan **Python Karar Motoru (Decision Engine)** tarafından işlenerek anlık konum kararlarının üretilmesini sağlayan uçtan uca bir IoT takip sistemidir.

---

## 📑 İçindekiler
1. [Sistem Mimarisi ve Çalışma Mantığı](#-sistem-mimarisi-ve-çalışma-mantığı)
2. [Bileşenler ve Klasör Yapısı](#-bileşenler-ve-klasör-yapısı)
3. [Algoritma ve Donanım Detayları](#-algoritma-ve-donanım-detayları)
   - [EMA (Üstel Hareketli Ortalama) Sinyal Filtresi](#1-ema-üstel-hareketli-ortalama-rssi-filtresi)
   - [Batarya Seviyesi Okuma ve Simülasyon Bilgisi](#2-batarya-seviyesi-okuma-ve-simülasyon-bilgisi)
   - [Dinamik MAC Tabanlı Client ID Üretimi](#3-dinamik-mac-tabanlı-client-id-üretimi)
4. [Sıfırdan Adım Adım Kurulum Kılavuzu](#-sıfırdan-adım-adım-kurulum-kılavuzu)
   - [Adım 1: Raspberry Pi, Docker ve Mosquitto Kurulumu](#adım-1-raspberry-pi-docker-ve-mosquitto-kurulumu)
   - [Adım 2: ESP32 IoT_Tag (Verici Etiket) Yükleme](#adım-2-esp32-iot_tag-verici-etiket-yükleme)
   - [Adım 3: ESP32 IoT_test (Alıcı/Tarayıcı Gateway) Yükleme](#adım-3-esp32-iot_test-alıcıtarayıcı-gateway-yükleme)
   - [Adım 4: Python Backend Karar Motorunu Çalıştırma](#adım-4-python-backend-karar-motorunu-çalıştırma)
5. [Sistemi Test Etme ve Doğrulama](#-sistemi-test-etme-ve-doğrulama)
6. [Karşılaşılan Sorunlar ve Çözümleri (Troubleshooting)](#-karşılaşılan-sorunlar-ve-çözümleri-troubleshooting)

---

## 🏗 Sistem Mimarisi ve Çalışma Mantığı

```mermaid
flowchart TD
    subgraph TagLayer ["1. Nesne Katmanı (BLE Beacons)"]
        Tag1["ESP32 IoT_Tag 01<br/>• 150ms Reklam Yayını<br/>• 30sn Deep Sleep<br/>• Tag ID & Pil Verisi"]
        Tag2["ESP32 IoT_Tag 02<br/>• Periyodik BLE Yayını"]
    end

    subgraph ScannerLayer ["2. Tarayıcı / Gateway Katmanı (ESP32)"]
        Scanner1["ESP32 Scanner #1 (Ofis Girişi)<br/>• Sürekli BLE Tarama (30ms)<br/>• EMA RSSI Filtresi (&alpha;=0.15)<br/>• Dinamik Client ID (MAC)<br/>• 1sn Hız Sınırlaması"]
        Scanner2["ESP32 Scanner #2 (Çalışma Alanı)<br/>• Sürekli BLE Tarama<br/>• EMA RSSI Filtresi<br/>• MQTT Yayıncı"]
    end

    subgraph ServerLayer ["3. Sunucu Katmanı (Raspberry Pi & Docker)"]
        Mosquitto["Eclipse Mosquitto Broker<br/>• Port: 1883 (TCP)<br/>• Şifreli Kimlik Doğrulama (passwd)"]
        Backend["Python Karar Motoru (Decision Engine)<br/>• Çoklu Scanner Veri Birleştirme<br/>• En Yakın Konum Çözümleme<br/>• Durum Değişim Takibi"]
        Portainer["Portainer CE<br/>• Konteyner Yönetim Arayüzü"]
    end

    Tag1 -->|BLE Reklam Paketi| Scanner1
    Tag1 -->|BLE Reklam Paketi| Scanner2
    Tag2 -->|BLE Reklam Paketi| Scanner1
    Tag2 -->|BLE Reklam Paketi| Scanner2

    Scanner1 -->|MQTT Pub: arge/test_device| Mosquitto
    Scanner2 -->|MQTT Pub: arge/test_device| Mosquitto

    Mosquitto <-->|MQTT Sub/Pub| Backend
    Backend -->|MQTT Pub: arge/decision (Retained)| Mosquitto
```

### Akış Özeti:
1. **IoT_Tag (Etiket)**: Belirli periyotlarla (varsayılan 30 saniye) derin uykudan (`esp_deep_sleep_start`) uyanır, üreticiye özel veri alanında (Manufacturer Data) etiket kimliğini (`TAG_ID`) ve pil yüzdesini içeren BLE reklam paketini 20 ms aralıklarla 150 ms boyunca çevreye yayınlar ve tekrar uyku moduna geçer.
2. **IoT_test (Tarayıcı Gateway)**: 2.4 GHz BLE bandını kesintisiz olarak dinler (Scan Window = Scan Interval = 30 ms). Beyaz listedeki (`WHITELIST_TAGS`) etiketlerden gelen ham sinyalleri yakalar, **EMA (Üstel Hareketli Ortalama)** filtresi ile radyo frekansı dalgalanmalarını sönümler, her etiket için saniyede maksimum 1 mesaj olacak şekilde hız sınırlaması (rate limiting) uygular ve JSON formatında MQTT konusuna (`arge/test_device`) iletir.
3. **Mosquitto MQTT Broker**: Raspberry Pi üzerinde Docker konteyneri olarak koşar. Şifreli kimlik doğrulama (`password_file`) ile çalışır.
4. **Python Karar Motoru**: Broker'a abone olarak tüm tarayıcılardan gelen RSSI verilerini hafızasında etiket bazlı matriste toplar. 3 saniyede bir çalışan karar döngüsü ile etiketin hangi tarayıcıya daha yakın olduğunu hesaplar ve `arge/decision` konusuna kalıcı (`retain=True`, `QoS=1`) olarak yazar.

---

## 📁 Bileşenler ve Klasör Yapısı

```text
esp32-ble-rssi-tracking/
├── docker-compose.yml                  # Mosquitto, Python Backend ve Portainer Docker yapılandırması
├── .env.example                        # Ortam değişkenleri şablonu (Kullanıcı adı, şifre, portlar)
├── .gitignore                          # Gizli dosyaları ve derleme artıklarını dışlama kuralları
├── README.md                           # Proje dokümantasyonu (Bu dosya)
│
├── mosquitto/                          # Mosquitto MQTT Broker yapılandırma klasörü
│   ├── config/
│   │   └── mosquitto.conf.example      # Mosquitto port ve yetkilendirme şablonu
│   ├── data/                           # Kalıcı MQTT veri dizini
│   └── log/                            # MQTT log dizini
│
├── python_backend/                     # Karar Motoru Servisi
│   ├── backend.py                      # Karar motoru Python kaynak kodu
│   ├── requirements.txt                # paho-mqtt, python-dotenv bağımlılıkları
│   ├── Dockerfile                      # Backend Docker imaj dosyası
│   ├── .env.example                    # Backend için örnek .env dosyası
│   └── .gitignore
│
├── IoT_Tag/                            # ESP-IDF Verici Etiket (Beacon) Projesi
│   ├── CMakeLists.txt
│   ├── main/
│   │   ├── CMakeLists.txt
│   │   └── IoT_Tag.c                   # BLE Beacon ve Deep Sleep kaynak kodu
│   └── .gitignore
│
└── IoT_test/                           # ESP-IDF Tarayıcı / Gateway Projesi
    ├── CMakeLists.txt
    ├── main/
    │   ├── CMakeLists.txt
    │   ├── idf_component.yml           # ESP-IDF MQTT ve kütüphane bağımlılıkları
    │   ├── credentials.h.example       # Wi-Fi ve MQTT kimlik bilgileri şablonu
    │   └── IoT_test.c                  # BLE Tarayıcı, EMA filtresi ve MQTT Gateway kodu
    └── .gitignore
```

---

## 🧠 Algoritma ve Donanım Detayları

### 1. EMA (Üstel Hareketli Ortalama) RSSI Filtresi
Kapalı ortamlarda 2.4 GHz radyo dalgaları; duvarlardan yansıma (multipath fading), insan hareketleri ve ortamdaki Wi-Fi kirliliği nedeniyle cihazlar sabit dursa dahi $\pm 15\text{ dBm}$'e varan anlık dalgalanmalar yaşar.

Bu dalgalanmayı engellemek ve işlemciyi/RAM'i yormamak için `IoT_test` kodunda **EMA Filtresi** kullanılmıştır:

$$\text{Filtered\_RSSI}_t = (\alpha \times \text{Raw\_RSSI}_t) + ((1 - \alpha) \times \text{Filtered\_RSSI}_{t-1})$$

- **$\alpha$ Değeri (Alpha = 0.15)**: Gelen ham veriye %15, geçmiş ortalamaya %85 ağırlık vererek anlık sıçramaları bıçak gibi keser ve stabil bir mesafe/konum hesabı sunar.

### 2. Batarya Seviyesi Okuma ve Simülasyon Bilgisi
> [!NOTE]
> **Prototip Notu:** Prototipleme ve laboratuvar testleri sırasında etiket cihazı doğrudan USB/geliştirme kartı üzerinden beslendiği ve henüz harici bir lityum pil voltaj bölücü devresi bağlanmadığı için `IoT_Tag.c` içerisindeki `battery_level` değeri simülatif olarak sabit (`85`) verilmiştir.

**Gerçek Donanımda Pil Okuma Entegrasyonu:**
Sistemi gerçek bir CR2032 düğme pil veya 3.7V Li-Po batarya ile çalıştırmak istediğinizde:
1. Bataryanın pozitif kutbu ile GND arasına iki adet eşit direnç (örn: $100\text{ k}\Omega / 100\text{ k}\Omega$) ile bir voltaj bölücü (Voltage Divider) bağlayın.
2. Dirençlerin orta noktasını ESP32'nin ADC pinine (örn: `ADC1_CHANNEL_0` / `GPIO 0`) bağlayın.
3. `IoT_Tag.c` dosyasına ADC okuma mantığını ekleyin:

```c
#include "esp_adc/adc_oneshot.h"

// Örnek ADC Okuma ve Batarya Yüzdesi Hesaplama Fonksiyonu
uint8_t read_battery_percentage(void) {
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&init_config1, &adc1_handle);

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &config);

    int raw_val = 0;
    adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &raw_val);
    adc_oneshot_del_unit(adc1_handle);

    // Voltaj bölücü ve batarya eğrisine göre yüzdeye (0-100) dönüştür
    float voltage = (raw_val / 4095.0f) * 3.3f * 2.0f; // 2x voltaj bölücü çarpanı
    int percentage = (int)(((voltage - 3.0f) / (4.2f - 3.0f)) * 100.0f);
    if (percentage > 100) percentage = 100;
    if (percentage < 0) percentage = 0;

    return (uint8_t)percentage;
}
```

### 3. Dinamik MAC Tabanlı Client ID Üretimi
Çok sayıda gateway cihazının aynı MQTT broker'a bağlanırken çakışmasını engellemek için her ESP32'nin fabrikasyon Wi-Fi MAC adresinin son 3 baytı (6 karakter) okunarak benzersiz bir kimlik oluşturulur:

```c
uint8_t mac[6];
esp_read_mac(mac, ESP_MAC_WIFI_STA);
char client_id[24];
snprintf(client_id, sizeof(client_id), "ESP_%02X%02X%02X", mac[3], mac[4], mac[5]);
// Çıktı Örneği: ESP_A1B2C3
```

---

## 🚀 Sıfırdan Adım Adım Kurulum Kılavuzu

### Adım 1: Raspberry Pi, Docker ve Mosquitto Kurulumu

1. **Gerekli Paketleri Kurun:**
   Raspberry Pi terminaline SSH ile bağlanın ve Docker ile Docker Compose'un kurulu olduğundan emin olun:
   ```bash
   sudo apt update && sudo apt install -y docker.io docker-compose mosquitto-clients
   sudo usermod -aG docker $USER
   ```

2. **Proje Dosyalarını Hazırlayın:**
   Projeyi Raspberry Pi üzerinde istediğiniz dizine (örn: `~/esp32-ble-rssi-tracking`) kopyalayın.

3. **Mosquitto Ayar Dosyasını Oluşturun:**
   `mosquitto/config/mosquitto.conf.example` dosyasını `mosquitto/config/mosquitto.conf` olarak kopyalayın:
   ```bash
   cp mosquitto/config/mosquitto.conf.example mosquitto/config/mosquitto.conf
   ```

4. **MQTT Şifre Dosyasını (`passwd`) Üretin:**
   Mosquitto konteyneri çalışmadan önce şifreli kullanıcı dosyasını üretmek için şu komutu çalıştırın (`YOUR_MQTT_USERNAME` ve `YOUR_MQTT_PASSWORD` değerlerini kendi belirleyeceğiniz bilgilerle değiştirin):
   ```bash
   docker run --rm -v $(pwd)/mosquitto/config:/tmp eclipse-mosquitto mosquitto_passwd -c -b /tmp/passwd YOUR_MQTT_USERNAME YOUR_MQTT_PASSWORD
   ```

5. **Klasör İzinlerini Düzeltin (ÖNEMLİ):**
   > [!IMPORTANT]
   > Docker içindeki Mosquitto resmi imajı `1883:1883` UID/GID kullanıcısı ile çalışır. Yetki uyuşmazlığı nedeniyle Mosquitto'nun çökme döngüsüne (Crash Loop) girmesini önlemek için izinleri ayarlayın:
   ```bash
   sudo chown -R 1883:1883 mosquitto/
   chmod 644 mosquitto/config/passwd
   ```

6. **Ortam Değişkenlerini Ayarlayın:**
   `python_backend/.env.example` dosyasını `python_backend/.env` olarak kopyalayın ve kimlik bilgilerinizi girin:
   ```bash
   cp python_backend/.env.example python_backend/.env
   nano python_backend/.env
   ```

7. **Docker Konteynerlerini Başlatın:**
   ```bash
   docker compose up -d --build
   ```
   Konteynerlerin durumunu kontrol edin:
   ```bash
   docker compose ps
   ```
   - **Portainer Arayüzü:** `http://<RASPBERRY_PI_IP>:9000` adresinden açılabilir.

---

### Adım 2: ESP32 IoT_Tag (Verici Etiket) Yükleme

1. **ESP-IDF Ortamını Yükleyin:**
   ESP-IDF v5.x ortamını terminalinizde etkinleştirin (`. $HOME/esp/esp-idf/export.sh`).
2. **Dizine Geçin ve Hedefi Ayarlayın:**
   ```bash
   cd IoT_Tag
   idf.py set-target esp32c6   # veya kullandığınız çipe göre: esp32, esp32s3, esp32c3
   ```
3. **Derleyin ve Flaşlayın:**
   ```bash
   idf.py build
   idf.py -p /dev/tty.usbserial-XXXX flash monitor
   ```

---

### Adım 3: ESP32 IoT_test (Alıcı/Tarayıcı Gateway) Yükleme

1. **Kimlik Dosyasını (`credentials.h`) Oluşturun:**
   `IoT_test/main/credentials.h.example` dosyasını `credentials.h` olarak kopyalayın:
   ```bash
   cp IoT_test/main/credentials.h.example IoT_test/main/credentials.h
   ```
2. **Bilgilerinizi Düzenleyin:**
   `IoT_test/main/credentials.h` dosyasını açıp Wi-Fi ve MQTT bilgilerinizi girin:
   ```c
   #define WIFI_SSID       "YOUR_WIFI_SSID"
   #define WIFI_PASS       "YOUR_WIFI_PASSWORD"
   #define BROKER_URI      "mqtt://192.168.1.50"  // Raspberry Pi yerel IP'si
   #define BROKER_USERNAME "YOUR_MQTT_USERNAME"
   #define BROKER_PASS     "YOUR_MQTT_PASSWORD"
   #define DEVICE_NAME     "WORK_AREA"            // Tarayıcının bulunduğu konum adı
   ```
3. **Derleyin ve Flaşlayın:**
   ```bash
   cd IoT_test
   idf.py set-target esp32c6
   idf.py build
   idf.py -p /dev/tty.usbserial-YYYY flash monitor
   ```

---

### Adım 4: Python Backend Karar Motorunu Çalıştırma

Eğer Raspberry Pi üzerinde Docker ile başlattıysanız servis zaten arka planda çalışmaktadır. Bağımsız olarak test etmek isterseniz:
```bash
cd python_backend
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python backend.py
```

---

## 🧪 Sistemi Test Etme ve Doğrulama

1. **MQTT Trafiğini Canlı İzleme:**
   Raspberry Pi terminalinde tüm gelen ham verileri ve karar çıktılarını canlı izlemek için:
   ```bash
   mosquitto_sub -h localhost -t "arge/#" -u "YOUR_MQTT_USERNAME" -P "YOUR_MQTT_PASSWORD" -v
   ```

2. **Gelen Veri Örnekleri:**
   - **Tarayıcıdan Gelen Ham Telemetri (`arge/test_device`):**
     ```json
     {"tag":"ARGE_TAG_01", "scanner":"WORK_AREA", "rssi":-58}
     ```
   - **Karar Motorunun Ürettiği Konum Bilgisi (`arge/decision`):**
     ```json
     {
       "tag": "ARGE_TAG_01",
       "location": "WORK_AREA",
       "rssi": -58,
       "timestamp": "18:45:12"
     }
     ```

3. **Backend Loglarını İnceleme:**
   ```bash
   docker logs -f iot_decision_engine
   ```

---

## 🛠 Karşılaşılan Sorunlar ve Çözümleri (Troubleshooting)

### 1. iPhone Hotspot ve "İstemci İzolasyonu" (Client Isolation) Tuzağı
- **Belirti:** ESP32 Wi-Fi'a bağlanıyor, IP alıyor ancak Raspberry Pi'deki MQTT sunucusuna bağlanırken `esp-tls select() timeout` (Hata: `32774`) veriyor.
- **Neden:** iPhone kişisel erişim noktası (Hotspot), güvenlik amacıyla aynı ağa bağlı cihazların birbirleriyle TCP/IP paketi alışverişi yapmasını engeller (AP Isolation).
- **Çözüm:** Testlerinizi standart bir ev/ofis Wi-Fi modemi (ADSL/Fiber) veya AP izolasyonu yapmayan bir Android erişim noktası üzerinden gerçekleştirin.

### 2. ESP-IDF MQTT URI Formatı
- **Belirti:** `transport_base: Failed to open a new connection`.
- **Neden:** ESP-IDF MQTT kütüphanesine adres sadece IP (`192.168.1.10`) olarak verilirse protokol ayrıştırma hatası oluşabilir.
- **Çözüm:** Adresin başına mutlaka `mqtt://` eklenmelidir (örn: `mqtt://192.168.1.50`). 1883 portu için `mqtts://` **kullanılmamalıdır**; aksi halde cihaz şifresiz porta TLS el sıkışması göndermeye çalışarak zaman aşımına uğrar.

### 3. Mosquitto Şifre Dosyası ve Dosya Yetkisi Çökmesi (Crash Loop)
- **Belirti:** `docker logs mosquitto` çıktısında `Unable to open password file` veya `Container is restarting` (409 Conflict).
- **Neden:** Konteyner içindeki `mosquitto` kullanıcısının (`UID 1883`) Pi üzerindeki klasörlere yazma/okuma izninin olmaması.
- **Çözüm:** `sudo chown -R 1883:1883 mosquitto/` komutu ile sahipliği düzeltip konteyneri yeniden başlatın (`docker restart mosquitto`).

### 4. ESP-IDF Wi-Fi Yeniden Bağlantıda Bellek Sızıntısı (Memory Leak)
- **Önlem:** `IoT_test.c` içerisinde `IP_EVENT_STA_GOT_IP` her tetiklendiğinde yeni bir MQTT client oluşturulmasını önlemek için `if (mqtt_client == NULL)` kontrolü eklenmiştir.

---

## 📄 Lisans
Bu proje açık kaynaklıdır. Detaylar için [LICENSE](LICENSE) dosyasına başvurabilirsiniz.
