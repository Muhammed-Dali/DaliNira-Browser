# PHASE 2.2D kabul kontrolü

Bu dosya otomatik test ile canlı site kabulünü ayrı kaydeder. Fixture sonuçları, YouTube veya oturum açılmış Facebook üzerindeki canlı kabulün yerine geçmez.

## Otomatik kontroller

| Gereksinimler | Kanıt | Durum |
|---|---|---|
| 1–7: tek tıklama, tekrar, keyboard, reopen, host, signal/state | `phase22d_test.cpp::shields` gerçek mouse/keyboard olayları ve model yazma sayısı | Geçti |
| 8–10: site reset izolasyonu, global factory reset | `phase22d_test.cpp::shields` | Geçti |
| 11–25: YouTube/Facebook DOM, recycled node, SPA, OFF/ON, observer | `phase22d_test.cpp::cosmetics` | Doğrulama sürüyor |
| 26–35, 41–43: consent, provider, parse, timeout, debounce, stale, dedupe, empty/private | `phase22d_test.cpp::suggestions` fake transport üzerinden gerçek QNetworkRequest yaşam döngüsü | Geçti |
| 36–40: iki arama yüzeyi, local/remote birleşimi, seçim, keyboard, favicon | BrowserWindow/New Tab çalışma zamanı kontrolü | Bekliyor |
| 44–47, 49: unsafe query/scheme, metadata endpoint, response sınırı | `phase22d_test.cpp::suggestions` | Geçti |
| 48, 51: active host güveni, normalization | native host kaynağı incelemesi; `phase22d_test.cpp::shields` | Kısmi; bridge saldırı testi bekliyor |
| 50: büyük filtre satırı | `phase22d_test.cpp::shields` | Geçti |
| 52: private/normal izolasyonu | ayrı profile service, interceptor, geçici dizin ve transfer profile kontrolü | Çalışma zamanı testi bekliyor |
| 53: query logging | ilgili kaynaklarda log incelemesi | Son denetim bekliyor |
| 54–55: observer/request resource sınırları | runtime ve suggestion testleri | Kısmi |

## Canlı endpoint kontrolü

2026-09-06: Sentetik kabul sorgusu `hav`, oturum cookie veya Authorization göndermeyen istekler:

- Google: HTTP 200, gerçek öneri dizisi.
- DuckDuckGo: HTTP 200, gerçek öneri dizisi.
- Bing: HTTP 200, gerçek öneri dizisi.
- Brave: HTTP 429. UI yerel sonuçları korumalı; başarı iddia edilmez.

## Manuel kabul

- [ ] A: youtube.com popup tüm switch’ler 10 ON/OFF, body/knob/label, Space/Enter.
- [ ] B: youtube.com site reset; facebook.com ve global korunuyor.
- [ ] C: Settings fabrika reset; açıklanan global filtre tercihleri, özel filtreler ve site istisnaları sıfırlanıyor.
- [ ] D: YouTube reload ×10, uzun scroll, home/video/home ×10, search/subscriptions dönüşleri, back/forward, tab switch, OFF/ON.
- [ ] D: video playback, controls, captions, fullscreen, comments, recommendations korunuyor.
- [ ] E: oturum açılmış Facebook feed uzun scroll; reklamlar gizlenirken normal post/group/marketplace/reel/comment/profile/notification/Messenger korunuyor.
- [ ] F: omnibox Google açık, `hav` uzak öneriler.
- [ ] G: öneriler kapalı, uzak istek yok.
- [ ] H: Google/DuckDuckGo/Brave/Bing geçişi; provider engeli ayrıca kaydediliyor.
- [ ] I: New Tab `eba`; dropdown/hover/ok tuşları/Enter/Escape; yerel sitelerde favicon.
- [ ] J: bridge endpoint/hostname spoof yok; unsafe scheme yok; private veriler normal profile yazılmıyor; query logging yok.

## Doğrulama komutları

```sh
./scripts/build-dalinira.sh
ctest --test-dir build-dalinira --output-on-failure
git diff --check
```

Release test hedeflerinde `-UNDEBUG` kullanılır. Testlere özel Chromium process seçenekleri ürünün sandbox ayarlarını değiştirmez. Sınırlı agent sandbox’ı WebEngine child process başlatamadığında testler dış süreç yetkisiyle çalıştırılır.
