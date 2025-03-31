// Copyright 2025 EasyStack, Inc.
// This file provides functions for requesting etcd services.

package main

import (
	"context"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	clientv3 "go.etcd.io/etcd/client/v3"
	"gopkg.in/yaml.v2"
)

// etcd client structure
type Client struct {
	*clientv3.Client
	Port      int
	Timeout   time.Duration
	Service   string
	Namespace string
	resolver  net.Resolver
}

// 配置结构体
type Config struct {
	Etcd struct {
		Endpoints []string `yaml:"endpoints"`
		LeaseTTL  int64    `yaml:"lease_ttl"`
	} `yaml:"etcd"`
}

// 默认配置
var defaultConfig = Config{
	Etcd: struct {
		Endpoints []string `yaml:"endpoints"`
		LeaseTTL  int64    `yaml:"lease_ttl"`
	}{
		Endpoints: []string{"localhost:2379"},
		LeaseTTL:  10,
	},
}

// 加载配置
func LoadConfig() (*Config, error) {
	config := defaultConfig

	// 从环境变量读取配置
	if endpoints := os.Getenv("ETCD_ENDPOINTS"); endpoints != "" {
		config.Etcd.Endpoints = strings.Split(endpoints, ",")
	}

	if ttl := os.Getenv("ETCD_LEASE_TTL"); ttl != "" {
		if parsedTTL, err := strconv.ParseInt(ttl, 10, 64); err == nil {
			config.Etcd.LeaseTTL = parsedTTL
		}
	}

	// 如果存在配置文件，则从配置文件读取
	if _, err := os.Stat("config.yaml"); err == nil {
		data, err := os.ReadFile("config.yaml")
		if err != nil {
			return nil, fmt.Errorf("读取配置文件失败: %v", err)
		}

		if err := yaml.Unmarshal(data, &config); err != nil {
			return nil, fmt.Errorf("解析配置文件失败: %v", err)
		}
	}

	return &config, nil
}

// 获取锁
func Lock(cli *clientv3.Client, key string, value string, ttl int64) error {
	lease := clientv3.NewLease(cli)
	leaseResp, err := lease.Grant(context.Background(), ttl)
	if err != nil {
		return fmt.Errorf("创建租约失败: %v", err)
	}

	// 使用事务确保原子性
	txn := cli.Txn(context.Background())
	txn.If(
		// 检查key不存在或value相同
		clientv3.Compare(clientv3.CreateRevision(key), "=", 0),
		// clientv3.Compare(clientv3.Value(key), "=", value),
	).Then(
		clientv3.OpPut(key, value, clientv3.WithLease(leaseResp.ID)),
	)

	txnResp, err := txn.Commit()
	if err != nil {
		return fmt.Errorf("获取锁失败: %v", err)
	}

	if !txnResp.Succeeded {
		txn := cli.Txn(context.Background())
		txn.If(
			// 检查key不存在或value相同
			// clientv3.Compare(clientv3.CreateRevision(key), "=", 0),
			clientv3.Compare(clientv3.Value(key), "=", value),
		).Then(
			clientv3.OpPut(key, value, clientv3.WithLease(leaseResp.ID)),
		)

		txnResp, err := txn.Commit()
		if err != nil {
			return fmt.Errorf("获取锁失败: %v", err)
		}

		if !txnResp.Succeeded {
			return fmt.Errorf("锁已被其他进程持有")
		}
	}

	return nil
}

// 续约
func KeepAlive(cli *clientv3.Client, key string, value string) error {
	resp, err := cli.Get(context.Background(), key)
	if err != nil {
		return fmt.Errorf("获取key失败: %v", err)
	}

	if len(resp.Kvs) == 0 {
		return fmt.Errorf("锁不存在")
	}

	// 检查value是否一致
	currentValue := string(resp.Kvs[0].Value)
	if currentValue != value {
		return fmt.Errorf("锁被其它进程%s持有，续约失败", currentValue)
	}

	leaseID := clientv3.LeaseID(resp.Kvs[0].Lease)
	lease := clientv3.NewLease(cli)
	_, err = lease.KeepAliveOnce(context.Background(), leaseID)
	if err != nil {
		return fmt.Errorf("续约失败: %v", err)
	}

	return nil
}

// 释放锁
func Unlock(cli *clientv3.Client, key string) error {
	resp, err := cli.Get(context.Background(), key)
	if err != nil {
		return fmt.Errorf("获取key失败: %v", err)
	}

	if len(resp.Kvs) == 0 {
		return fmt.Errorf("锁不存在")
	}

	leaseID := clientv3.LeaseID(resp.Kvs[0].Lease)
	lease := clientv3.NewLease(cli)
	_, err = lease.Revoke(context.Background(), leaseID)
	if err != nil {
		return fmt.Errorf("撤销租约失败: %v", err)
	}

	return nil
}

// 查询锁的持有者
func GetLockOwner(cli *clientv3.Client, key string) (string, error) {
	resp, err := cli.Get(context.Background(), key)
	if err != nil {
		return "", fmt.Errorf("查询锁持有者失败: %v", err)
	}

	if len(resp.Kvs) == 0 {
		return "", fmt.Errorf("锁不存在")
	}

	return string(resp.Kvs[0].Value), nil
}

// HTTP处理函数
// TODO: how to use the etcd client of each service
func setupRoutes(r *gin.Engine, cli *clientv3.Client, config *Config) {
	r.POST("/lock/:key", func(c *gin.Context) {
		key := c.Param("key")
		value := c.PostForm("value")

		if err := Lock(cli, key, value, config.Etcd.LeaseTTL); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
			return
		}

		c.JSON(http.StatusOK, gin.H{"message": "锁定成功"})
	})

	r.POST("/keepalive/:key", func(c *gin.Context) {
		key := c.Param("key")
		value := c.PostForm("value")

		if err := KeepAlive(cli, key, value); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
			return
		}

		c.JSON(http.StatusOK, gin.H{"message": "续约成功"})
	})

	r.POST("/unlock/:key", func(c *gin.Context) {
		key := c.Param("key")

		if err := Unlock(cli, key); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
			return
		}

		c.JSON(http.StatusOK, gin.H{"message": "解锁成功"})
	})

	// 添加查询锁持有者的路由
	r.GET("/lock/:key", func(c *gin.Context) {
		key := c.Param("key")

		owner, err := GetLockOwner(cli, key)
		if err != nil {
			c.JSON(http.StatusBadRequest, gin.H{
				"error": err.Error(),
			})
			return
		}

		// 返回锁的持有者信息
		c.JSON(http.StatusOK, gin.H{
			"key":   key,
			"owner": owner,
		})
	})

	// 添加查询所有锁的路由（可选）
	r.GET("/locks", func(c *gin.Context) {
		// 使用前缀查询获取所有锁
		resp, err := cli.Get(context.Background(), "", clientv3.WithPrefix())
		if err != nil {
			c.JSON(http.StatusInternalServerError, gin.H{
				"error": fmt.Sprintf("查询失败: %v", err),
			})
			return
		}

		locks := make([]map[string]string, 0)
		for _, kv := range resp.Kvs {
			locks = append(locks, map[string]string{
				"key":   string(kv.Key),
				"owner": string(kv.Value),
			})
		}

		c.JSON(http.StatusOK, gin.H{
			"locks": locks,
		})
	})
}

// Create a new etcd client
func NewClient(ctx context.Context, port int, timeout time.Duration, service, namespace string) (*Client, error) {
	var err error

	if timeout == 0 {
		timeout = 60 * time.Second
	}
	if namespace == "" {
		namespace = "default"
	}

	client := &Client{
		Timeout:   timeout,
		Port:      port,
		Service:   service,
		Namespace: namespace,
		resolver: net.Resolver{
			PreferGo: true,
			Dial: func(ctx context.Context, network, address string) (net.Conn, error) {
				d := net.Dialer{}
				return d.DialContext(ctx, network, address)
			},
		},
	}

	endpoints := client.LocalhostEndpoints()
	if service != "" {
		endpoints, err = client.LookupEndpoints(ctx)
		if err != nil {
			return nil, err
		}
	}

	client.Client, err = clientv3.New(clientv3.Config{
		Endpoints:   endpoints,
		DialTimeout: timeout,
	})
	if err != nil {
		return nil, err
	}

	return client, nil
}

// Get the localhost endpoints
func (c *Client) LocalhostEndpoints() []string {
	return c.ParseUrls([]string{"127.0.0.1"})
}

func (c *Client) ParseUrls(addrs []string) []string {
	urls := make([]string, len(addrs))
	for i, addr := range addrs {
		u := url.URL{
			Scheme: "http",
			Host:   fmt.Sprintf("%s:%d", addr, c.Port),
		}
		urls[i] = u.String()
	}
	return urls
}

func (c *Client) LookupEndpoints(ctx context.Context) ([]string, error) {
	cctx, cancel := context.WithTimeout(ctx, c.Timeout)
	defer cancel()

	ips, err := c.resolver.LookupHost(cctx, c.DnsName())
	if err != nil {
		return nil, fmt.Errorf("failed to lookup DNS names: %w", err)
	}
	return c.ParseUrls(ips), nil
}

func (c *Client) DnsName() string {
	return fmt.Sprintf("%s.%s.svc.cluster.local", c.Service, c.Namespace)
}

func main() {
	// 加载配置
	config, err := LoadConfig()
	if err != nil {
		log.Printf("加载配置失败，使用默认配置: %v", err)
		config = &defaultConfig
	}

	// 打印当前配置
	log.Printf("当前配置 - Endpoints: %v, LeaseTTL: %d",
		config.Etcd.Endpoints, config.Etcd.LeaseTTL)

	// 创建etcd客户端
	// TODO: use NewClient function to create etcd client for each service
	cli, err := clientv3.New(clientv3.Config{
		Endpoints:   config.Etcd.Endpoints,
		DialTimeout: 5 * time.Second,
	})
	if err != nil {
		log.Fatal(err)
	}
	defer cli.Close()

	// 创建gin路由
	r := gin.Default()
	setupRoutes(r, cli, config)
	r.Run(":8080")
}
