# VKey v4.3

[![VKeyClassic binaries signed by SignPath](https://img.shields.io/badge/SignPath-VKeyClassic_binaries_only-blue)](https://signpath.io)

> ⚠️ **Lưu ý quan trọng về Chữ ký số & Cập nhật tự động (Code Signing & Auto-Update)**
>
> 1. **Phạm vi ký Foundation**: Trong phiên bản v4.3, SignPath Foundation chỉ ký ba binary GPL-3.0 của gói Classic chính thức: `VKeyClassic.exe`, `VKeyTSF.dll` và `VKeyWatchdog.exe`. `VKey.exe`/VKeyApp, `sciter.dll`, installer, Engine Rust (`vkey_engine.dll`) và mọi thành phần proprietary đều nằm ngoài phạm vi ký này. `VKeyBrowserHost.exe` cũng là mã nguồn GPL-3.0 công khai nhưng không nằm trong request ba file hiện tại. Bản tiêu chuẩn (Sciter UI) chưa được ký Authenticode nên có thể bị Windows SmartScreen hoặc phần mềm diệt virus cảnh báo (chọn *More info* → *Run anyway* để tiếp tục sử dụng).
> 2. **Bản Classic chính thức là 100% mã nguồn mở (Open Source)**: Ba binary được ký được build cùng nhau từ cấu hình Foundation riêng, với Rust bị compile-out hoàn toàn và không include, link, load, bundle, install hoặc download Rust/Sciter. Biến thể Classic + Rust trên manual workflow chỉ dành cho maintainer kiểm thử, không ký số và không được phát hành.
> 3. **Cần tải bản v4.3 thủ công**: Do v4.2 yêu cầu kiểm tra chữ ký số an toàn khi nâng cấp, tính năng cập nhật tự động từ v4.2 lên v4.3 (bản tiêu chuẩn) sẽ không hoạt động. Người dùng đang ở phiên bản v4.2 vui lòng **tải và cài đặt bản v4.3 thủ công** từ trang phát hành GitHub Release.

Bản cập nhật này mang đến cải tiến đột phá về khả năng tự động sửa lỗi chính tả cùng các sửa lỗi quan trọng giúp nâng cao độ ổn định.

### ✨ Tính năng mới & Cải tiến nổi bật

* **🧪 VKey Browser — tự chuyển chế độ theo website (thử nghiệm)**
    * Thêm extension đồng hành [VKey-Browser](https://github.com/phatMT97/VKey-Browser) và `VKeyBrowserHost.exe` để áp dụng **Theo VKey**, **English** hoặc **TSF tương thích** theo từng tên miền.
    * Chuyển tab hoặc chuyển cửa sổ sẽ tự cập nhật chế độ. Ví dụ có thể dùng V trên `google.com` và tự chuyển E trên `facebook.com` mà không làm thay đổi trạng thái V/E gốc.
    * Có công tắc bật/tắt toàn cục: tắt sẽ tạm dừng mọi rule nhưng vẫn giữ nguyên cấu hình để bật lại sau.
    * Popup hiển thị trạng thái kết nối native host và dùng icon VKey; gói Chromium/Firefox có manifest riêng để tương thích Manifest V3.
    * Chế độ TSF theo tên miền hỗ trợ các editor/forum Firefox bị dính chữ sau emoji/inline image; composition được đặt kiểu hiển thị không gạch chân.
    * Extension chỉ gửi hostname, browser, trạng thái focus và route cục bộ; không gửi URL đầy đủ, nội dung trang hoặc phím gõ.
    * Hiện cài bằng Developer mode/Load unpacked và chưa phát hành trên Chrome Web Store hoặc Firefox AMO. Extension không thể đọc nội dung đang gõ trong address bar trước khi navigation.
    * Hướng dẫn cài đặt: [docs/BROWSER_EXTENSION.md](docs/BROWSER_EXTENSION.md).
* **🌐 Ra mắt trang thông tin chính thức**: Chính thức ra mắt website tại [www.vkey.qd.je](https://www.vkey.qd.je/) giúp người dùng dễ dàng tra cứu thông tin tính năng, hướng dẫn sử dụng và tải về các bản phát hành.
* **Tinh chỉnh UI**  (chỉ ở giao diện hiện đại)
    * **Tab "Hệ thống"**: Gom nhóm tuỳ chọn icon để giúp UI gọn hơn
    * **Tab "Macro"**: Tách riêng các nút hành động (Lưu, Test, Xóa, Nhập) và cải thiện bố cục danh sách macro để dễ sử dụng hơn.
    * **Xóa nhiều gõ tắt cùng lúc**: Thêm ô chọn ở bảng Macro để chọn và xóa nhiều từ gõ tắt trong một lần.
    * **Quản lý "Từ điển & Loại trừ chính tả"**: Bổ sung hộp thoại đồ họa trực quan để quản lý Từ điển cá nhân (`user_dictionary.txt`) và Ngoại lệ viết tắt; hỗ trợ thêm, sửa, xóa, nạp/xuất file `.txt` và lưu áp dụng tức thì. Tab được thiết kế chia đều 50/50, phân tách rõ hàng thao tác tệp và hàng nút hành động.
* **Chế độ "Kiểm tra chính tả nâng cao"**
    * **Engine Rust hiệu năng cao**: Bổ sung engine kiểm tra chính tả mới được viết bằng Rust, tập trung vào hiệu năng và khả năng nhận diện lỗi.
    * **Tự sửa lỗi gõ nhanh**: Phát hiện và sửa các lỗi gõ nhanh, đảo ký tự hoặc nhầm ký tự (ví dụ: `hcaof` → `chào`, `xywr` → `xử`).
    * **Gõ từ tiếng Anh & thương hiệu mượt mà**: Có thể viết các từ như `asus` mà không cần gõ 3 chữ `s` (`assus`) hay thao tác phục hồi phức tạp — chỉ cần gõ đúng thứ tự `a-s-u-s` là engine tự nhận diện và xuất đúng từ.
    * **Hoạt động theo lựa chọn của người dùng**: Tính năng mặc định được tắt và chỉ được kích hoạt khi bạn chủ động bật trong cài đặt.
    * **Tương thích với engine mã nguồn mở**: Nếu engine nâng cao không có hoặc bị gỡ bỏ, VKey sẽ tự động sử dụng engine C++ mã nguồn mở đi kèm mà không ảnh hưởng đến các chức năng gõ tiếng Việt thông thường.

> ℹ️ **Tìm hiểu thêm**
>
> Để biết thêm chi tiết về kiến trúc engine nâng cao, cơ chế phân phối, lý do thiết kế cũng như các vấn đề về mã nguồn & giấy phép (license), vui lòng tham khảo:
>
> - [Kiến trúc kỹ thuật Engine nâng cao](docs/ENGINE_ARCHITECTURE.md)
> - [Giải đáp FAQ về kiến trúc, giấy phép & nguồn gốc VKey](docs/ENGINE_FAQ.md)

* **🎮 Chế độ game (thay đổi hành vi mặc định — game thủ vui lòng đọc)**
    * **Trước đây**: cơ chế giữ phím cho game được bật ngầm cho **mọi** ứng dụng Win32 thông thường (Notepad, Word, Windows Terminal…). Mỗi lần bỏ dấu, VKey nhả ký tự gốc ra màn hình rồi mới xoá đi. Việc này gây nháy chữ và tình trạng đôi lúc không đặt được dấu đúng mong đợi.
    * **Từ bản này**: mặc định mọi ứng dụng dùng cách gõ thông thường (xoá rồi thay). Cơ chế giữ phím cho game trở thành **tuỳ chọn**, bật riêng cho từng ứng dụng.
    * **Cách bật**: chọn `Chế độ game` trong mục "Cách gửi" ở **Cấu hình từng ứng dụng**, hoặc gán một phím tắt cho `Bật / tắt chế độ game cho app đang mở` trong **Quản lý phím tắt** rồi nhấn ngay khi đang ở trong game — ứng dụng đó sẽ được thêm vào danh sách, nhấn lần nữa để gỡ.
    * Phím tắt này **không có tổ hợp mặc định**: bất cứ tổ hợp nào chọn sẵn cũng sẽ đụng phím đã gán trong game, nên bạn tự chọn.
    * Ứng dụng nào đã cấu hình sẵn "Cách gửi" khác (Clipboard, Firefox, Cloud/Remote…) không bị ảnh hưởng.
---

### 🛠 Các lỗi đã được khắc phục
*   **Sửa lỗi bỏ dấu từ ghép**: Khắc phục lỗi gõ từ `ruouwj` không bỏ dấu đúng cách để tạo thành từ `rượu`.
*   **Sửa lỗi gõ từ tiếng Anh**: Khắc phục hiện tượng gõ từ `view` bị chuyển nhầm thành `vieư`.
*   **Sửa lỗi nạp từ điển cá nhân (User Dictionary)**: Khắc phục lỗi engine chính tả Rust từ chối file `user_dictionary.txt` do xung đột cú pháp ghi chú header (`;` thay vì `#`), khiến các từ bảo vệ đã lưu (như `soà`, `khưm`...) vẫn bị tự sửa thành từ khác khi bật Kiểm tra chính tả nâng cao.
*   **Tối ưu hóa trạng thái hoạt động**: Sửa lỗi ứng dụng chuyển sang trạng thái chờ (idle) quá nhanh gây ảnh hưởng đến trải nghiệm người dùng.
*   **Cải tiến giao diện**: Tối ưu hóa hiệu năng hiển thị và chuyển đổi của các bộ giao diện (theme).
*   **Sửa lỗi excel online**: Khắc phục lỗi mất từ trước đó khi dùng shift để viết hoa từ tiếng Việt. 
*   **Sửa lỗi macro TSF**: Macro đã hoạt động với TSF
*   **Sửa lỗi mất dấu không hồi phục được**: Khắc phục lỗi thỉnh thoảng gõ `khoong` ra thẳng `khoong` thay vì `không`, và sau đó xoá đi gõ lại vẫn không tạo được dấu.

---

**Sponsors**

Free code signing for the three approved VKeyClassic binaries is provided by [SignPath.io](https://signpath.io/), certificate by [SignPath Foundation](https://signpath.org/). Thank you, SignPath! 🙏

---

*Cảm ơn bạn đã tin tưởng và lựa chọn VKey! Mọi đóng góp của bạn đều là nguồn động lực lớn giúp bộ gõ ngày càng hoàn thiện hơn.* ❤️
