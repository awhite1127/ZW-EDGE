package service

// 本文件集中控制页面聚合查询的并发启动和结果收集，避免各页面重复维护 goroutine 样板。

import (
	"context"
	"sync"
)

type loadResult[T any] struct {
	value T
	err   error
}

// startLoad 启动一个独立数据源查询；调用方在读取结果前必须等待 group。
func startLoad[T any](ctx context.Context, group *sync.WaitGroup, load func(context.Context) (T, error)) *loadResult[T] {
	result := new(loadResult[T])
	group.Add(1)
	go func() {
		defer group.Done()
		result.value, result.err = load(ctx)
	}()
	return result
}
