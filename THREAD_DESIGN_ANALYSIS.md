# xv6 Thread 설계/구현 심층 분석

## 1. 개요
이 코드베이스는 전통적인 xv6의 `proc = process` 모델을, **thread group + shared address space** 모델로 확장했다.
핵심 아이디어는 다음과 같다.

- 커널 스케줄링 단위는 여전히 `struct proc`.
- 그러나 여러 `proc`가 하나의 주소공간(`vmspace`)을 공유할 수 있다.
- `clone()`은 같은 `vmspace`를 참조하는 새 실행 흐름(스레드)을 만든다.
- `join()`은 같은 thread-group 내 종료 스레드를 회수(reap)한다.

즉, "프로세스 테이블 기반 커널 스레드 + 공유 VM" 형태의 단순하고 교육용으로 적합한 설계다.

---

## 2. 데이터 구조 확장

### 2.1 `struct vmspace` (proc.h)
`vmspace`는 thread group 단위의 주소공간 메타데이터를 묶는다.

- `pgdir`: 그룹 공용 페이지 테이블
- `sz`: 사용자 메모리 크기
- `ref`: 공유 참조 카운트
- `vmlock`: 주소공간 크기/매핑 변경 동기화
- `joinchan`: join sleep/wakeup 채널 토큰

이 구조 덕분에 기존 xv6의 `proc->pgdir`, `proc->sz`를 분리해, 다중 실행 흐름이 동일 VM을 자연스럽게 참조할 수 있다.

### 2.2 `struct proc` 확장 필드
- `isthread`: 스레드 여부
- `tid`: 스레드 ID (현재 구현에서는 `pid`와 동일하게 부여)
- `tgid`: thread group ID (리더 pid)
- `mainthread`: 그룹 리더 여부
- `ustack`: 사용자 스택 주소(주로 join 반환)
- `retval`: 확장 여지(현 시점에서 pthread-like return 경로는 미완성)
- `vm`: 공유 `vmspace` 포인터

`pid`와 `tid`를 분리해 둔 점은 향후 POSIX 유사 의미론 확장에 유리하다.

---

## 3. 생성 경로

### 3.1 `fork()`
기존 xv6와 동일하게 별도 주소공간을 만들되, 내부 표현이 `vmspace` 기반으로 바뀌었다.

- 새 `vmspace` 할당
- 부모 `pgdir` 복사(`copyuvm`)
- `np->vm->ref = 1`

중요 포인트: fork child는 thread가 아니라 독립 process이며, 따라서 `wait()` 대상으로 유지된다.

### 3.2 `clone(void (*fcn)(void*), void *arg, void *stack)`
스레드 생성은 다음 순서로 동작한다.

1. `allocproc()`로 실행 컨텍스트/커널스택 확보
2. 부모의 `vmspace` 공유 + `ref++`
3. trapframe 복제 후 사용자 스택 수동 구성
   - `arg`
   - fake return address (`0xffffffff`)
4. `eip = fcn`, `esp = user stack top`
5. FD/CWD 참조 복제
6. `isthread=1`, `tgid` 부모와 동일

이는 Linux `clone`과 유사한 방향이지만, 플래그 기반 세밀 공유정책(CLONE_VM/FILES/SIGHAND)은 단순화되어 있다.

---

## 4. 종료/회수 경로

### 4.1 `exit()`
현재 구현은 `vmspace->ref`를 기준으로 두 경로를 분기한다.

- **마지막 스레드가 아닌 경우**: VM만 detach(`ref--`), 자기 자신을 ZOMBIE로 두고 `join()` 대상이 됨
- **마지막 스레드인 경우**: 전통 xv6 종료 경로로 파일/ cwd 정리 및 고아 자식 처리

즉, process-level 자원 정리는 "마지막 thread"가 수행한다.

### 4.2 `join(void **stack)`
동일 `tgid`의 `isthread` 엔트리 중 ZOMBIE를 찾아 reap한다.

- 종료 스레드의 커널스택 해제
- 프로세스 슬롯 초기화 (`UNUSED`)
- 필요시 user stack 주소 반환

이 구현은 pthread_join의 가장 핵심인 "종료 thread 회수"를 제공한다.

---

## 5. 동기화 및 메모리 일관성 분석

### 5.1 `vmlock` 역할
`sbrk/growproc` 같은 주소공간 크기 변경 경로에서 보호를 제공한다.
스레드가 동일 `pgdir/sz`를 공유하므로 필수적이다.

### 5.2 `ptable.lock`와 thread lifecycle
스레드 상태 전이(RUNNABLE, ZOMBIE, UNUSED), join/wait sleep-wakeup은 `ptable.lock` 질서에 의존한다.

- `join`은 `sleep(&vm->joinchan, &ptable.lock)`
- thread exit 시 `wakeup1(&vm->joinchan)`

기존 xv6 wait/wakeup 패턴을 thread 그룹 회수에 재사용한 구조다.

### 5.3 이번 수정에서 보강한 부분
1. `fork()` 실패 경로에서 `vmspace` 해제 누락 수정 (`vmspacefree` 호출)
2. `clone()`에서 `vmspace->ref++`를 `ptable.lock` 보호 하에 수행
3. userland `thread_create()` 실패 경로(UAF 위험) 개선
   - stack registry 실패 시 기존 스택 즉시 free 대신 `tkill(pid)`로 정리 시도

---

## 6. 사용자 라이브러리 계층 (`ulib.c`)

### 6.1 `thread_create`
- 2페이지 할당 후 page align
- 상단 page 끝 주소를 clone stack으로 전달
- `threadstacks[]`에 raw malloc 포인터 보관

이는 `join`으로 받는 stack(top)과 `free`에 필요한 raw 포인터가 다르다는 점을 해결하기 위한 매핑 레이어다.

### 6.2 `thread_join`
- `join(&stack)`으로 종료 thread pid 획득
- pid 기반으로 raw stack 포인터를 찾아 `free`

구현은 단순/직관적이지만, `threadstacks[]`가 고정크기/락없음이라 고경합 환경에선 취약할 수 있다.

---

## 7. 시스템콜 의미론

- `kill(pid)`: 해당 pid가 속한 tgid 전체 kill 전파
- `tkill(tid)`: 단일 thread kill

즉, process kill과 thread kill을 분리해 인터페이스를 제공한다.
이는 디버깅과 제어 관점에서 매우 유용한 확장이다.

---

## 8. 잠재 리스크 / 추가 개선 제안

1. `threadstacks[]` 동기화 부재
   - 멀티스레드가 동시에 `thread_create/thread_join` 호출 시 경쟁 가능
2. `clone` 인자 검증 강화
   - 스택 정렬/경계 조건(과제 요구사항에 따라 페이지 단위 검증) 명시 강화 가능
3. join 대상 정책
   - 현재는 같은 tgid의 임의 종료 스레드 수거. 특정 tid join 모델로 확장 가능
4. 반환값 (`retval`) 경로
   - pthread_exit / thread return value 수집 모델 미완성

---

## 9. 결론
이 구현은 xv6 교육 목적에 맞게, 복잡도를 억제하면서도 스레드 핵심 개념(공유 주소공간, 독립 실행흐름, join 기반 회수)을 잘 반영한다.

특히 `vmspace` 분리와 refcount 기반 종료 정책은 설계적으로 타당하며, 기존 xv6의 간결한 락/스케줄러 구조와 자연스럽게 결합된다.

이번 보강으로 실패 경로 안정성과 참조 카운트 경쟁 조건이 개선되어, thread 구현의 실전 안정성이 한 단계 높아졌다.
