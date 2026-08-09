-- ============================================================================
-- rate_limit.lua — 滑动窗口限流 Lua 脚本
--
-- 用法：
--   EVAL "<script>" 1 limit:user:1001:/api/order <now_ms> <window_ms> <member> <max_req> <ttl_sec>
--
-- 返回值：
--   1 = 放行
--   0 = 拒绝
-- ============================================================================

local now     = tonumber(ARGV[1])
local window  = tonumber(ARGV[2])
local member  = ARGV[3]
local max_req = tonumber(ARGV[4])
local ttl     = tonumber(ARGV[5])

-- Step 1: 清理窗口外的过期成员
redis.call('ZREMRANGEBYSCORE', KEYS[1], 0, now - window)

-- Step 2: 先检查当前计数
-- 优化：如果已经满了，就不做 ZADD（减少内存操作）
local count = redis.call('ZCARD', KEYS[1])

if count >= max_req then
    redis.call('EXPIRE', KEYS[1], ttl)
    return 0  -- 拒绝
end

-- Step 3: 记录本次请求
redis.call('ZADD', KEYS[1], now, member)

-- Step 4: 设置过期（防止冷 key 常驻内存）
redis.call('EXPIRE', KEYS[1], ttl)

return 1  -- 放行
