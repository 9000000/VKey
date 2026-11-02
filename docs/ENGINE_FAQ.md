# Giải đáp Thắc mắc về VKey Engine (FAQ)

Tài liệu này giải đáp các câu hỏi thường gặp liên quan đến bộ xử lý gõ tiếng Việt (Engine), tính năng Kiểm tra Chính tả Nâng cao, bảo mật, quyền riêng tư và giấy phép của dự án **VKey**.

---

## Mục lục

1. [Tính năng & Trải nghiệm gõ](#1-tính-năng--trải-nghiệm-gõ)
2. [An toàn, Bảo mật & Quyền riêng tư](#2-an-toàn-bảo-mật--quyền-riêng-tư)
3. [Quản lý, Cài đặt & Gỡ bỏ](#3-quản-lý-cài-đặt--gỡ-bỏ)
4. [Mã nguồn & Giấy phép phát hành](#4-mã-nguồn--giấy-phép-phát-hành)

---

## 1. Tính năng & Trải nghiệm gõ

### VKey Engine là gì?
VKey Engine là "bộ não" xử lý quy tắc ngôn ngữ của bộ gõ. Khi bạn gõ các phím từ bàn phím (như `v`, `i`, `e`, `t`, `s`), Engine có nhiệm vụ phân tích và chuyển đổi chuỗi phím này thành từ tiếng Việt hoàn chỉnh có dấu (`việt`).

### VKey có mấy bộ xử lý (engine)? Sự khác biệt giữa Engine C++ và Engine Rust Nâng cao là gì?
VKey được thiết kế theo mô hình **Engine kép (Dual-Engine)** gồm hai bộ xử lý:

| Tiêu chí | Engine C++ (Mặc định) | Engine Rust Nâng cao (Tùy chọn) |
| :--- | :--- | :--- |
| **Mã nguồn** | Mã nguồn mở 100% tích hợp sẵn | Thư viện mở rộng động (`vkey_engine.dll`), tạm thời closed-source |
| **Phiên bản hỗ trợ** | Cả bản Tiêu chuẩn (Sciter UI) & bản Classic | Chỉ có trên bản Tiêu chuẩn (khi người dùng chủ động bật) |
| **Kiểu gõ** | Telex, VNI, Simple Telex chuẩn | Telex, VNI nâng cao + Tự sửa lỗi |
| **Kiểm tra chính tả** | Cơ bản (theo luật âm vị học tĩnh) | Nâng cao (từ điển âm tiết + phân tích ngữ cảnh) |
| **Tự sửa lỗi gõ nhanh** | Không hỗ trợ | Tự sửa lỗi đảo ký tự hoặc nhầm ký tự (`hcaof` → `chào`, `xywr` → `xử`) |
| **Gõ từ tiếng Anh (asus...)** | Cần gõ thoát hoặc lặp phím (`assus`) | Tự động nhận diện (`a-s-u-s` → `asus`) |
| **Cách cài đặt** | Có sẵn trong ứng dụng VKey | Tùy chọn bật trong Cài đặt |

**Chi tiết sự khác biệt:**
- **Engine C++ (Built-in C++ Engine)**: Là engine cốt lõi, 100% mã nguồn mở và được tích hợp trực tiếp trong VKey. Engine này xử lý việc bỏ dấu tiếng Việt dựa trên các quy tắc ngữ âm học chuẩn (phụ âm đầu, nguyên âm ghép, thanh điệu, phụ âm cuối). Ưu điểm là siêu nhẹ, tốc độ xử lý tức thì, ổn định tuyệt đối và có mặt trên tất cả các phiên bản (kể cả bản Classic).
- **Engine Rust Nâng cao (Advanced Rust Engine)**: Là module thư viện mở rộng (`vkey_engine.dll`), được viết bằng ngôn ngữ Rust hiện đại nhằm tối ưu hóa an toàn bộ nhớ và hiệu năng xử lý từ vựng. Engine này trang bị thêm từ điển âm tiết và các thuật toán nhận diện lỗi gõ thông minh, giúp tự động sửa lỗi đảo ký tự hoặc nhầm ký tự khi gõ lướt phím tốc độ cao và hỗ trợ gõ mượt mà các từ tiếng Anh / thương hiệu phổ biến.

### Chế độ "Kiểm tra chính tả nâng cao" khác gì với "Kiểm tra chính tả cơ bản"?
Hai chế độ này phục vụ các mục tiêu khác nhau trong trải nghiệm gõ:

1. **Kiểm tra chính tả cơ bản (Basic Spell Check - trên Engine C++)**:
   - **Nguyên lý hoạt động**: Dựa trên bộ quy tắc kết hợp âm vị học tiếng Việt (cấu trúc ngữ âm tĩnh: phụ âm đầu hợp lệ + nguyên âm hợp lệ + thanh điệu + phụ âm cuối hợp lệ).
   - **Tác dụng**: Ngăn chặn việc bỏ dấu sai vào các từ không hợp lệ hoặc các từ tiếng Anh chứa phím dấu/mũ Telex (ví dụ: gõ `for`, `star` không bị bỏ nhầm dấu hỏi thành `fỏ`, `stả`; gõ `show` không bị biến thành `shơ`).
   - **Hạn chế**: Khi bạn gõ phím quá nhanh dẫn đến đảo ký tự hoặc nhầm ký tự (như `hcaof`, `xywr`), chế độ cơ bản sẽ coi đó là từ sai luật và giữ nguyên hoặc cho ra kết quả không mong muốn. Với các từ tiếng Anh có kết thúc bằng phím dấu (như `asus` có `s` là dấu sắc trong Telex), bạn thường phải gõ lặp phím `s` (`assus`) hoặc dùng phím khôi phục/thoát từ để giữ nguyên dạng `asus`.

2. **Kiểm tra chính tả nâng cao (Advanced Spell Check - trên Engine Rust)**:
   - **Nguyên lý hoạt động**: Sử dụng Engine Rust kết hợp từ điển âm tiết tiếng Việt và thuật toán phân tích mẫu lỗi gõ (typing pattern & transposition correction).
   - **Tự sửa lỗi gõ nhanh / đảo ký tự hoặc nhầm ký tự**: Tự động nhận diện và sửa lại đúng các từ bị đảo thứ tự hoặc nhầm ký tự do thao tác lướt phím nhanh (ví dụ: `hcaof` → tự động sửa thành `chào`, `xywr` → `xử`).
   - **Gõ từ tiếng Anh & thương hiệu mượt mà**: Nhận diện thông minh các từ tiếng Anh hoặc tên thương hiệu thông dụng như `asus`, `status`, `plus`... Bạn chỉ cần gõ đúng chuỗi ký tự theo thứ tự `a-s-u-s` là engine tự nhận biết và xuất ra `asus` mà không bị biến thành `asú` hay bắt buộc phải gõ 3 chữ `s` (`assus`).
   - **Trải nghiệm gõ liền mạch**: Giúp bạn gõ tự nhiên ở tốc độ cao mà không bị khựng lại hay phải liên tục nhấn Backspace để sửa các lỗi đảo phím thường gặp.

### Tôi có bắt buộc phải cài đặt hay dùng Engine Rust Nâng cao không?
**Không**. VKey hoạt động hoàn toàn độc lập và đáp ứng 100% nhu cầu gõ tiếng Việt thông thường chỉ với Engine C++ mặc định. Bạn có thể sử dụng VKey ngay sau khi tải về mà không cần bật hay cài đặt thêm bất kỳ thành phần nào.

### Nếu tôi bật Kiểm tra Chính tả Nâng cao mà file engine bị thiếu hoặc bị lỗi thì sao?
VKey có cơ chế **tự động chuyển đổi (Fallback)** thông minh: nếu file `vkey_engine.dll` không tồn tại, bị lỗi, hoặc thiếu file chữ ký `vkey_engine.dll.sig` đi kèm, VKey sẽ ngay lập tức quay lại sử dụng Engine C++ mặc định. Quá trình gõ tiếng Việt của bạn sẽ không bao giờ bị ngắt quãng.

### Tại sao sau khi cập nhật VKey, tính năng nâng cao tạm thời chưa hoạt động trong Chrome / Word?
**Cách xử lý nhanh**: Bạn chỉ cần **đóng hẳn ứng dụng đó (Chrome, Word,...) rồi mở lại**, hoặc khởi động lại máy là tính năng sẽ hoạt động bình thường.

**Lý do**: Do Windows giữ khóa các file hệ thống khi Chrome/Word đang mở, nên VKey cần ứng dụng được mở lại để nạp phiên bản cập nhật mới nhất.

*📌 Lưu ý:* Trong suốt quá trình này, việc gõ tiếng Việt của bạn vẫn hoạt động hoàn toàn bình thường (không bị gián đoạn), chỉ tạm thời không áp dụng tính năng sửa lỗi gõ nhanh cho đến khi ứng dụng được mở lại.

---

## 2. An toàn, Bảo mật & Quyền riêng tư

### VKey hay Engine Rust có theo dõi hoặc gửi phím gõ của tôi ra Internet không?
**Hoàn toàn KHÔNG**. 
- Cả ứng dụng VKey và thư viện Engine Rust đều chạy **100% ngoại tuyến (offline)** trực tiếp trên máy tính của bạn.
- Thư viện Engine Rust là một file DLL chạy nội bộ trong tiến trình của VKey và **không chứa bất kỳ mã lệnh kết nối mạng nào**.

### Làm sao tôi có thể tự kiểm chứng VKey không gửi dữ liệu bàn phím đi đâu?
Bạn có thể tự kiểm tra bằng 2 cách đơn giản sau:
1. **Dùng Tường lửa (Windows Firewall)**: Bạn có thể tạo quy tắc chặn hoàn toàn kết nối Internet của file `VKey.exe`. VKey và tính năng gõ tiếng Việt / kiểm tra chính tả vẫn hoạt động hoàn hảo 100%.
2. **Kiểm tra mã nguồn**: Toàn bộ mã nguồn phần tiếp nhận bàn phím, giao tiếp thư viện và hiển thị giao diện của VKey đều được mở công khai trên GitHub để bất kỳ ai cũng có thể đọc và audit.

### Vì sao VKey phải kiểm tra `vkey_engine.dll` trước khi nạp?
Vì file này chạy **bên trong** ứng dụng bạn đang gõ. Nếu nạp bừa, ai đặt được một file cùng tên vào thư mục cài VKey là chạy được mã của họ trong mọi ô nhập liệu của bạn. Nên VKey chỉ nạp engine khi chứng minh được nó đúng là bản do dự án phát hành.

### VKey kiểm bằng cách nào?
Bằng **chữ ký số**. Mỗi bản engine phát hành đi kèm một file nhỏ `vkey_engine.dll.sig` (72 byte) — chữ ký ECDSA P-256 do dự án ký. Khoá công khai để kiểm chữ ký đó được nhúng sẵn trong `VKey.exe` và `VKeyTSF.dll` lúc biên dịch, nên không ai thay được từ bên ngoài. Trước khi nạp, VKey tự tính lại mã băm của engine trên đĩa rồi đối chiếu với chữ ký; sai một byte là từ chối.

Vì vậy **`vkey_engine.dll.sig` phải luôn nằm cạnh `vkey_engine.dll`** — chép engine đi đâu thì chép cả file này theo, thiếu nó VKey sẽ quay về Engine C++.

### Chữ ký này bảo vệ được tới đâu?
Nó chặn mọi cách tráo **riêng file engine**: thay bằng file khác, tự ký bằng khoá lạ, sửa số hiệu trong file chữ ký, hay đắp lại bản cũ — đều bị từ chối.

Nó **không** bảo vệ được khi máy bạn đã bị chiếm quyền ghi vào thư mục cài VKey: kẻ sửa được `VKeyTSF.dll` thì cũng sửa được chính đoạn mã đi kiểm tra. Không cơ chế nào ở tầng này ngăn được điều đó — hãy giữ máy sạch và chỉ tải VKey từ trang phát hành chính thức.

## 3. Quản lý, Cài đặt & Gỡ bỏ

### Tại sao file `vkey_engine.dll` không được đóng gói sẵn trong file ZIP phát hành?
Để giữ cho bộ cài VKey dung lượng **nhẹ nhất có thể** (chỉ vài MB) cho những người dùng chỉ có nhu cầu gõ tiếng Việt cơ bản. Người dùng nào muốn trải nghiệm tính năng kiểm tra chính tả nâng cao mới bật tùy chọn để ứng dụng tải về.

### Làm thế nào để bật hoặc tắt Kiểm tra Chính tả Nâng cao?
1. Mở bảng điều khiển VKey từ khay hệ thống (System Tray).
2. Lựa chọn **Kiểm tra chính tả nâng cao**.

### Làm sao để chế độ Nâng cao luôn giữ một từ riêng của tôi?
Sau lần đầu bật chế độ Nâng cao, mở file `user_dictionary.txt` nằm cạnh
`config.toml`, báo lỗi tự sửa tại [GitHub Issues](https://github.com/phatMT97/VKey/issues),
rồi thêm mỗi từ cần giữ vào một dòng và lưu file. Chọn lại **Nâng cao** (hoặc
chuyển Nâng cao → Cơ bản → Nâng cao) để nạp lại cho cả Hook và TSF. File không
tồn tại sẽ được VKey tạo sẵn; file trống là một danh sách hợp lệ và xóa mọi từ
đã bảo vệ. Nếu file sai UTF-8, có dòng không hợp lệ hoặc tạm thời không đọc
được, VKey giữ nguyên bản hợp lệ gần nhất. File này chỉ được đọc cục bộ và
không được gửi ra ngoài.

### Làm thế nào để gỡ bỏ hoàn toàn Engine Rust?
Mở thư mục cài đặt VKey và xóa hai file: `vkey_engine.dll` và `vkey_engine.dll.sig`.

Sau khi xóa, VKey sẽ tự động quay về sử dụng Engine C++ mặc định mà không cần cấu hình gì thêm.

### Máy tính của tôi không có mạng, tôi có thể cài đặt Engine Rust thủ công không?
**Có**. Tải **cả hai** file `vkey_engine.dll` và `vkey_engine.dll.sig` từ trang [GitHub Release](https://github.com/phatMT97/VKey/releases) chính thức bằng một máy khác, rồi chép cả hai vào cùng thư mục chứa `VKey.exe`. Thiếu file `.sig` thì VKey không xác thực được engine và sẽ dùng Engine C++.

---

## 4. Mã nguồn & Giấy phép phát hành

### VKey được phát hành theo giấy phép mã nguồn mở nào?
VKey được phát hành dưới giấy phép mã nguồn mở **GNU General Public License v3.0 (GPL-3.0)**.

### Trong thư mục cài có mấy file license, chúng là gì?
Hai file, và chúng nói về hai thứ khác nhau:

| File | Nội dung |
| :--- | :--- |
| `THIRD_PARTY_NOTICES.txt` | Danh sách thư viện của bên thứ ba đi kèm VKey (Sciter…) và các thông báo mà giấy phép của họ bắt buộc phải kèm theo bản phân phối, chứ không chỉ nằm trong mã nguồn. |
| `vkey_engine.LICENSE.txt` | Giấy phép riêng của Engine Rust nâng cao. Engine **không** thuộc giấy phép GPL-3.0 của VKey — xem câu dưới. |

Chúng đi kèm bản tải về vì chính các giấy phép đó yêu cầu thông báo phải đi cùng file nhị phân. Bạn không cần làm gì với hai file này, nhưng đừng xoá khi chia sẻ lại bản cài cho người khác.

### Công ty tôi cài VKey cho nhân viên dùng có sao không?
**Không sao cả.** GPL-3.0 ràng buộc việc *phân phối* phần mềm, không ràng buộc việc *dùng*. Cài VKey cho nhân viên gõ tiếng Việt là sử dụng bình thường: không phải công khai gì, không phải xin phép, và không ảnh hưởng tới bất kỳ phần mềm nội bộ nào của công ty — VKey là ứng dụng chạy độc lập, không nhúng vào sản phẩm của bạn.

Nghĩa vụ chỉ phát sinh nếu bạn **phát hành lại** VKey ra bên ngoài (nguyên bản hoặc đã sửa): khi đó phải kèm mã nguồn tương ứng theo GPL-3.0.

Lưu ý riêng cho Engine nâng cao: xem câu ngay dưới.

### Tôi dùng VKey cho mục đích thương mại (bán kèm, dịch vụ) được không?
- **Bản thân VKey**: được, theo điều khoản GPL-3.0. GPL không cấm thương mại — nó chỉ buộc kèm mã nguồn khi bạn phân phối phần mềm.
- **Engine Rust nâng cao**: **không**. Engine nhúng từ điển âm tiết tiếng Việt dẫn xuất từ corpus Leipzig `vie_news_2020_1M` phát hành theo **CC BY-NC** (phi thương mại). Quyền thương mại với dữ liệu đó không thuộc về tác giả VKey nên không thể cấp cho bạn, và điều khoản này không thể xin miễn trừ.

Nếu bạn cần dùng thương mại: tắt **Kiểm tra Chính tả Nâng cao** và xoá `vkey_engine.dll` + `vkey_engine.dll.sig`. VKey vẫn gõ tiếng Việt đầy đủ bằng Engine C++ tích hợp. Nếu bạn fork mã nguồn, build với `-DVKEY_USE_RUST_ENGINE=OFF` và không phát hành lại các file engine.

### Phần nào của VKey là mã nguồn mở?
Toàn bộ mã nguồn cốt lõi của VKey bao gồm:
- Hạ tầng tiếp nhận sự kiện bàn phím từ hệ điều hành (Windows TSF IME & Low-Level Hook).
- Hệ thống điều phối sự kiện (Coordinator) và giao diện người dùng (Sciter UI).
- Engine xử lý tiếng Việt C++ (Built-in C++ Engine).
- Cơ chế nạp động an toàn, xác thực chữ ký số ECDSA P-256 và giao diện C ABI.

Tất cả đều được công khai minh bạch tại repository GitHub của dự án.

### Phiên bản VKey Classic có sử dụng được Engine Rust Nâng cao không?
**Không, đối với bản Classic phát hành chính thức.** Phiên bản **VKey Classic là phiên bản 100% mã nguồn mở (Open Source)**, thuần Win32 native siêu nhẹ. Phương án chỉ ký `VKeyClassic.exe`, `VKeyTSF.dll` và `VKeyWatchdog.exe` bằng SignPath Foundation đang **chờ phê duyệt lại**; Classic v4.3 vẫn phát hành chưa ký cùng Sciter trong lúc chờ. Cả ba được build cùng nhau từ cấu hình Foundation riêng, compile-out Rust hoàn toàn và không có Sciter; `VKeyBrowserHost.exe` không thuộc phạm vi đề xuất này. SignPath không chấp thuận ký bản tiêu chuẩn vì Sciter là thành phần mã nguồn đóng, và đợt phát hành này không gửi package tới SignPath. Maintainer có một cờ opt-in để thử Classic với Rust khi phát triển, nhưng binary thử nghiệm đó không phải sản phẩm Classic chính thức, không được gửi tới SignPath và không được phát hành dưới tên `VKeyClassic.zip`.

### Tôi là lập trình viên, tôi có thể tự viết Engine riêng hoặc tích hợp VKey với bộ xử lý khác không?
**Có, nhưng phải tự build VKey.** Giao diện C ABI là chuẩn mở (`include/vkey_engine.h`), viết engine tuân thủ nó là đủ về mặt kỹ thuật.

Tuy nhiên **bản VKey phát hành sẵn sẽ không nạp engine của bạn**: nó chỉ chấp nhận engine mang chữ ký của dự án, và đó chính là thứ ngăn người khác đặt một DLL tuỳ ý vào thư mục cài trên máy người dùng. Để chạy engine tự viết, bạn cần build VKey từ mã nguồn với khóa công khai (Public Key) do bạn tự tạo (`extern/vkey_engine/engine.pub`).

Chi tiết kỹ thuật trong [ENGINE_ARCHITECTURE.md](ENGINE_ARCHITECTURE.md).

---

## Tài liệu Liên quan

- **[ENGINE_ARCHITECTURE.md](ENGINE_ARCHITECTURE.md)**: Chi tiết kiến trúc kỹ thuật, luồng xử lý phím, C ABI và mô hình bảo mật file.
