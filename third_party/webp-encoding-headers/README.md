# Skia 내장 libwebp용 공개 헤더

Skia `0873ec164a06966b90ae0d43ef783cfb180084ae`의 DEPS에 고정된 libwebp 리비전
`845d5476a866141ba35ac133f856fa62f0b7445f`의 공개 헤더 네 개와 BSD 라이선스를 보관한다.

원본은 [libwebp 고정 소스](https://chromium.googlesource.com/webm/libwebp/+/845d5476a866141ba35ac133f856fa62f0b7445f/),
헤더 경로는 `src/webp/encode.h`, `mux.h`, `mux_types.h`, `types.h`다.
`source.json`에 원본 경로·리비전·다운로드 해시와 로컬 CRLF 해시를 기록했다.
줄 끝 정규화와 `COPYING` 마지막 빈 줄 제거 외 내용은 수정하지 않았다. `COPYING`은 원본 BSD 라이선스다.

추가 libwebp 바이너리를 빌드·다운로드·복사하지 않는다. `luil::skia`가 이미 링크하는 libwebp 구현의
`WebPAnimEncoder`와 `WebPConfig`를 직접 사용하기 위해 이 헤더만 추가했다.
Skia 의존성을 갱신할 때 이 헤더의 리비전도 확인해야 한다. 런타임 초기화에서 libwebp 헤더 ABI 호환성을
검사하지만 이것이 모든 다른 버전의 의미상 호환성을 보장하지 않는다.
