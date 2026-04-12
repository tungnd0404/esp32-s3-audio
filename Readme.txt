cấu trúc chương trinh:
khởi động: chạy hình ảnh boot (logo,...)
hiển thị menu dánh sách phát đọc từ file scv
cấu trúc nut bấm:
next: phát bài tiếp hoặc đang ở menu thì di chuyển con trỏ xuống
prev: phát bài trước hoặc đang ở menu thì di chuyển con trỏ lên
play/pause: phát/tạm dừng bài hát hiện tại hoặc đang ở menu thì chọn bài. 2 lần bấm để về menu

cấu trúc state:
menu
- state up
- state down
- state select
play
- state playing
- state paused
- state next
- state prev

cấu trúc trạng thái hoạt động: 
khi đang phát nhạc. bấm 2 lần play/pause để quay lại menu nhưng nhạc vẫn chạy. oled chuyển sang giao diện menu 5s rồi đổi lại giao diện frame




chỉ cần 1 buffer lưu frame. sd task đọc thẻ nhớ sau 50ms. thuật toán virtualTime thay đổi thì sd đọc. khi ghi xong thì báo cho oled qua 1 biến ready. oled check biến và phát luôn. nên đọc trước sd tầm 5ms virtualTime + 5ms