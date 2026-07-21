<<<<<<< HEAD
# first
=======
# ACU (Access Control Unit)

임베디드 리눅스 기반 출입보안 장치. 화면 없는(headless) CLI 데몬으로 동작한다.

## 개발 단계

- **[0단계] 데몬 뼈대** ← 현재 여기
- 1단계: SQLite3 + 출입 판정 로직
- 2단계: 하드웨어 추상화 계층(HAL) 인터페이스 확정
- 3단계: config.json 감시 + 무중단 리로드
- 4단계: 웹 설정 인터페이스 (Flask)
- 5단계: 네트워크 통신부 (TCP/UDP)
- 6단계: 실제 하드웨어 (GPIO / Wiegand)

## 빌드 & 실행

```bash
make        # 빌드 -> acud 생성
make run    # 빌드 후 실행
make clean  # 산출물 삭제
```

실행 후 다른 터미널에서:

```bash
kill -HUP  <pid>   # 설정 리로드 신호 (동작 확인용)
kill -TERM <pid>   # 정상 종료
```

또는 실행 중인 터미널에서 `Ctrl+C` (SIGINT) 로도 정상 종료된다.
>>>>>>> e5a1270 (0단계: 데몬 뼈대)
