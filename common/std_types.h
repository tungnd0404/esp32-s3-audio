#ifndef STD_TYPES_H
#define STD_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Kiểu trả về chuẩn cho các hàm biểu thị thành công/thất bại, thay cho bool true/false -
   dùng chung cho TOÀN BỘ project (phong cách AUTOSAR Std_ReturnType) */
typedef uint8_t Std_ReturnType;

#define E_OK        ((Std_ReturnType)0u)
#define E_NOT_OK    ((Std_ReturnType)1u)

#ifdef __cplusplus
}
#endif

#endif /* STD_TYPES_H */
