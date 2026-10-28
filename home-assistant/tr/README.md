# Jamiryo - Türkçe Home Assistant kurulumu

Bu klasör birbiriyle eşleşen **Türkçe** settir. Ana klasördeki İngilizce dosyalarla
karıştırma. Ev profilini robota kaydetmek paneli HA'ya otomatik kurmaz.

1. Jamiryo kurulumunda evin 2,4 GHz Wi-Fi bilgilerini, kendi Gemini anahtarını,
   yerel HA adresini ve HA erişim anahtarını kaydet. Ev Wi-Fi'sine geri dön.
2. **Ayarlar → Cihazlar ve Hizmetler → Yardımcılar** bölümünde oluştur:

   | Tür | Entity kimliği | Ayar |
   | --- | --- | --- |
   | Metin | `input_text.jamiryo_message` | En az 0, en fazla 140, normal metin |
   | Metin | `input_text.jamiryo_inbox` | En az 0, en fazla 255, normal metin |
   | Açılır liste | `input_select.jamiryo_mode` | `Jamiryo’ya sor`, `Evde sesli okut`, `Fotoğraf gönder` |

   İki metin alanını ilk kurulumda bir kez boş başlat. Gelen kutusunu normal
   kullanımda elle düzenleme. Otomatik son ek alan entity kimliklerini düzelt.
3. **Otomasyonlar ve Sahneler → Betikler → Yeni betik → YAML düzenle** bölümüne
   [jamiryo_send.yaml](jamiryo_send.yaml) içeriğini yapıştır. Ad **Jamiryo gönder**,
   entity kimliği **`script.jamiryo_gonder`** olmalı.
4. Ayrı, boş **Jamiryo** panosu oluştur. URL yolu **`jamiryo-mesaj`** olsun.
   **Düzenle → menü → Ham yapılandırma düzenleyicisi** bölümüne
   [jamiryo_dashboard.yaml](jamiryo_dashboard.yaml) içeriğini yapıştır. Görünüm
   yolu `sohbet` kalır. `sensor.jamiryo_messenger` robot tarafından oluşturulur;
   bunu yardımcı olarak ekleme.
5. Resmi HA Companion uygulamasını bu eve bağla; bildirimleri aç. Jamiryo'nun
   kurulumunda **Fotoğraflı bildirim → Telefonları bul → Bu telefona gönder**
   ile doğru telefonu seç. Ev Wi-Fi'sine dön ve panelin hazır olmasını bekle.
6. Kısa soru, sesli okutma ve **Fotoğraf iste** denemesi yap. Sesi gerçekten
   dinle; telefonda bildirimi ve fotoğrafı kontrol et. Panelin tamamlandı demesi
   tek başına teslim kanıtı değildir.

Kullanım: **Metni yaz → Ne yapsın? → Mesajı gönder**. Formdan ayrılarak veya
Enter/Tamam ile metni kaydet. Tek mesaj gönder düğmesi vardır; fotoğraf kısayolu
ayrıdır. İşlenirken tekrar tekrar gönderme. Döküm gelmezse istenen metin açıklamayla
gösterilir; bu, sesin bağımsız yazılı dökümü değildir.

Deneyimli YAML kullanıcıları, 2 ve 3 yerine [paket yöntemini](../package-install.md)
bu klasördeki [jamiryo_package.yaml](jamiryo_package.yaml) ile kullanabilir.
UI ve paket yöntemlerini birlikte kurma. Paket, HA yeniden başladığında iki
metin alanını boşaltır. Hiçbir YAML dosyasına gerçek anahtar veya şifre yazma.

Ev dışında telefonun HA'ya Tailscale gibi mevcut özel erişimle ulaşmalıdır;
Jamiryo VPN kurmaz. Her yeni HA sunucusunda kurulum tekrarlanır. Ev seçimi
eski kişisel notları/yüzleri silmez; yeni sahibine teslim ayrı temizlik gerektirir.

Ayrıntılar ve sorun giderme: [İngilizce rehber](../README.md).
