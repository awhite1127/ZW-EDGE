package httpserver

// 本文件按客户端键记录短期登录失败次数，并用有限容量和过期清理防止暴力尝试及内存无界增长。

import (
	"net"
	"strings"
	"sync"
	"time"
)

const (
	loginFailureLimit      = 5
	loginLockDuration      = 5 * time.Minute
	loginFailureRecordTTL  = 5 * time.Minute
	maxLoginAttemptEntries = 1024
)

type loginAttemptEntry struct {
	failures    int
	lastFailure time.Time
	lockedUntil time.Time
}

type loginAttemptStore struct {
	mu      sync.Mutex
	entries map[string]loginAttemptEntry
	now     func() time.Time
}

// newLoginAttemptStore 创建登录失败次数存储。
func newLoginAttemptStore() *loginAttemptStore {
	return &loginAttemptStore{
		entries: make(map[string]loginAttemptEntry),
		now:     time.Now,
	}
}

// loginAttemptKey 生成稳定的对象标识。
func loginAttemptKey(remoteAddr string) string {
	remoteAddr = strings.TrimSpace(remoteAddr)
	if remoteAddr == "" {
		return ""
	}
	host, _, err := net.SplitHostPort(remoteAddr)
	if err != nil || strings.TrimSpace(host) == "" {
		return remoteAddr
	}
	return strings.TrimSpace(host)
}

// isLocked 判断是否为锁定。
func (store *loginAttemptStore) isLocked(key string) bool {
	if store == nil {
		return false
	}
	now := store.now()

	store.mu.Lock()
	defer store.mu.Unlock()

	store.cleanupExpiredLocked(now)
	entry, ok := store.entries[key]
	return ok && entry.lockedUntil.After(now)
}

// recordFailure 记录一次登录失败并更新锁定状态。
func (store *loginAttemptStore) recordFailure(key string) (int, bool) {
	if store == nil {
		return 0, false
	}
	now := store.now()

	store.mu.Lock()
	defer store.mu.Unlock()

	store.cleanupExpiredLocked(now)
	if _, exists := store.entries[key]; !exists && len(store.entries) >= maxLoginAttemptEntries {
		store.evictOldestLocked()
	}
	entry := store.entries[key]
	entry.failures++
	entry.lastFailure = now
	locked := false
	if entry.failures >= loginFailureLimit {
		entry.lockedUntil = now.Add(loginLockDuration)
		locked = true
	}
	store.entries[key] = entry
	return entry.failures, locked
}

// evictOldestLocked 在容量达到上限时淘汰最久未失败的客户端记录。
func (store *loginAttemptStore) evictOldestLocked() {
	oldestKey := ""
	var oldestTime time.Time
	for key, entry := range store.entries {
		referenceTime := entry.lastFailure
		if entry.lockedUntil.After(referenceTime) {
			referenceTime = entry.lockedUntil
		}
		if oldestKey == "" || referenceTime.Before(oldestTime) {
			oldestKey = key
			oldestTime = referenceTime
		}
	}
	if oldestKey != "" {
		delete(store.entries, oldestKey)
	}
}

// clear 清除指定客户端的登录失败记录。
func (store *loginAttemptStore) clear(key string) {
	if store == nil {
		return
	}
	store.mu.Lock()
	defer store.mu.Unlock()
	delete(store.entries, key)
}

// cleanupExpiredLocked 在持锁状态下清理已过期的登录失败记录。
func (store *loginAttemptStore) cleanupExpiredLocked(now time.Time) {
	for key, entry := range store.entries {
		if !entry.lockedUntil.IsZero() {
			if !entry.lockedUntil.After(now) {
				delete(store.entries, key)
			}
			continue
		}
		if !entry.lastFailure.IsZero() && !entry.lastFailure.Add(loginFailureRecordTTL).After(now) {
			delete(store.entries, key)
		}
	}
}
