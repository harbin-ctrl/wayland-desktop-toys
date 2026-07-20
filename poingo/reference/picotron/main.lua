--[[pod_format="raw",created="2025-10-03 03:00:37",modified="2025-10-12 02:59:21",revision=295,xstickers={}]]
-- poingo.lua
-- A nice Poing ball!  Only Picotron makes it fantasy!

local inside = {}
local base_u = {}
local base_u_fp = {}
local base_u_int = {}
local v_int = {}
local spanL = {}
local spanR = {}

function build_frame_pair_shared(phase_i)
  local phase_rad = (phase_i / FRAME_COUNT) * ANGLE_PERIOD
  local phase_u_fp = math.floor(((phase_rad / two_pi) * LON_TILES) * 256 + 0.5)

  local runs_by_row = {}
  local L, D = LIGHT_SHADE, DARK_SHADE

  for y = 0, BALL_H - 1 do
    local xl, xr = spanL[y], spanR[y]
    if xl then
      local row = {}
      local baseu_fp_row = base_u_fp[y]
      local v_row = v_int[y]

      local u0 = (baseu_fp_row[xl] + phase_u_fp) >> 8
      local c0 = (((u0 + v_row[xl]) & 1) == 0) and L or D
      local run_start = xl

      for x = xl + 1, xr do
        local u = (baseu_fp_row[x] + phase_u_fp) >> 8
        local c = (((u + v_row[x]) & 1) == 0) and L or D
        if c ~= c0 then
          row[#row+1] = { run_start, x - 1, c0 }
          run_start, c0 = x, c
        end
      end
      row[#row+1] = { run_start, xr, c0 }
      runs_by_row[y] = row
    end
  end

  local udA = userdata("u8", BALL_W, BALL_H)
  local udB = userdata("u8", BALL_W, BALL_H)

  set_draw_target(udA)
  for y = 0, BALL_H - 1 do
    local row = runs_by_row[y]
    if row then
      for i = 1, #row do
        local x0, x1, c = row[i][1], row[i][2], row[i][3]
        rectfill(x0, y, x1, y, c)
      end
    end
  end

  set_draw_target(udB)
  for y = 0, BALL_H - 1 do
    local row = runs_by_row[y]
    if row then
      for i = 1, #row do
        local x0, x1, c = row[i][1], row[i][2], row[i][3]
        local ci = (c == L) and D or L
        rectfill(x0, y, x1, y, ci)
      end
    end
  end

  set_draw_target()

  local j = (phase_i + HALF_PERIOD) % FRAME_COUNT
  ball_frames[phase_i] = udA
  ball_frames[j] = udB

  return udA, udB
end

function precompute_tables()
  local cx, cy = BALL_R - 0.5, BALL_R - 0.5
  
  for y = 0, BALL_H - 1 do
    inside[y], base_u[y], base_u_fp[y], v_int[y] = {}, {}, {}, {}

    local dy = (y - cy)
    local rem = BALL_R * BALL_R - dy * dy
    if rem >= 0 then
      local dx = math.sqrt(rem)
      spanL[y] = math.ceil (cx - dx)
      spanR[y] = math.floor(cx + dx)
    else
      spanL[y], spanR[y] = nil, nil
    end

    local vy2 = (y - cy) / BALL_R
    local vy2_sq = vy2 * vy2

    for x = 0, BALL_W - 1 do
      local vx2 = (x - cx) / BALL_R
      local r2 = vx2 * vx2 + vy2_sq

      if r2 <= 1.0 then
        local nz = math.sqrt(1.0 - r2)
        local nx, ny = vx2, vy2

        local NA = nx*Ax + ny*Ay + nz*Az
        local NU = nx*Ux + ny*Uy + nz*Uz
        local NV = nx*Vx + ny*Vy + nz*Vz

        local lat = math.asin(math.max(-1, math.min(1, NA)))
        local v = math.floor(((lat + (math.pi * 0.5)) / math.pi) * LAT_TILES)

        local lon0 = math.atan(NV, NU)
        local u_cols = ((lon0 + math.pi) / two_pi) * LON_TILES

        inside[y][x] = true
        base_u[y][x] = u_cols
        base_u_fp[y][x] = math.floor(u_cols * 256 + 0.5)
        v_int[y][x] = v
      else
        inside[y][x] = false
      end
    end
  end
end

function _init()
  local config = fetch("/appdata/poingo/config.pod")
  if not config then
    config = {
      make_noise = true,
      fullscreen = false
    }
    mkdir("/appdata/poingo")
    store("/appdata/poingo/config.pod", config)
  end
  
  show_settings_panel = false
  make_noise = config.make_noise
  fullscreen = config.fullscreen or false
  fullscreen_initial = fullscreen
  fullscreen_changed = false
  
  if fullscreen then
    window()
  else
    window {
      width = 320,
      height = 180,
      title = "Poingo",
      has_frame = true,
      moveable = true,
      resizeable = true,
      background_updates = true,
      background_draws = true
    }
  end
  
  last_mb = 0

  CANV_W, CANV_H = 480, 270
  BALL_W, BALL_H = 124, 124
  BALL_R = BALL_W * 0.5

  LON_TILES = 16
  LAT_TILES = 8

  FPS = 60
  ROT_PERIOD = 0.5
  GRAV = 0.02
  SECONDS = ROT_PERIOD * (LON_TILES / 2)
  FRAMES_N = FPS * SECONDS

  ball_x, ball_y = 100, 80
  vx, vy = 1.0, -1.6

  GRAV_FRAC = GRAV / 180
  lastDH = 180

  LIGHT_SHADE = 17
  DARK_SHADE = 16

  PERIOD_FRAMES = math.max(1, math.floor(FRAMES_N * (2 / LON_TILES) + 0.5))

  phase_f, phase_i = 0, 0
  ADV_PER_TICK = PERIOD_FRAMES / (ROT_PERIOD * FPS)

  two_pi = math.pi * 2
  four_pi = math.pi * 4
  ANGLE_PERIOD = four_pi / LON_TILES

  local tilt_deg = 20
  local s, c = math.sin(math.rad(tilt_deg)), math.cos(math.rad(tilt_deg))

  Ax, Ay, Az = s, c, 0

  local Hx, Hy, Hz = 0, 0, 1
  if math.abs(Ax * Hx + Ay * Hy + Az * Hz) > 0.99 then
    Hx, Hy, Hz = 1, 0, 0
  end

  Ux = Hy * Az - Hz * Ay
  Uy = Hz * Ax - Hx * Az
  Uz = Hx * Ay - Hy * Ax
  local ulen = math.sqrt(Ux * Ux + Uy * Uy + Uz * Uz)
  Ux, Uy, Uz = Ux / ulen, Uy / ulen, Uz / ulen

  Vx = Ay * Uz - Az * Uy
  Vy = Az * Ux - Ax * Uz
  Vz = Ax * Uy - Ay * Ux

  ball_frames = {}
  FRAME_COUNT = PERIOD_FRAMES
  HALF_PERIOD = math.floor(FRAME_COUNT / 2 + 0.5)
  precompute_tables()
end

function _update()
  local toggle_key = false
  
  for i = string.byte('a'), string.byte('z') do
    if keyp(string.char(i)) then toggle_key = true break end
  end
  if not toggle_key then
    for i = string.byte('0'), string.byte('9') do
      if keyp(string.char(i)) then toggle_key = true break end
    end
  end
  if keyp("space") or keyp("return") then
    toggle_key = true
  end
  
  if toggle_key then
    show_settings_panel = not show_settings_panel
  end
  
  local mx, my, mb = mouse()
  
  if show_settings_panel then
    if mb > 0 and last_mb == 0 then
      handle_panel_click(mx, my)
    end
  end
  
  last_mb = mb
end

function handle_panel_click(mx, my)
  local disp = get_display()
  local DW, DH = disp:width(), disp:height()
  
  local pw = math.min(200, DW - 40)
  local ph = math.min(120, DH - 40)
  local px = (DW - pw) / 2
  local py = (DH - ph) / 2
  
  local btn_w = 120
  local btn_h = 20
  local btn_x = px + (pw - btn_w) / 2
  local btn_y = py + ph - 26
  if mx >= btn_x and mx <= btn_x + btn_w and
     my >= btn_y and my <= btn_y + btn_h then
    close_settings_panel()
    return
  end
  
  local cb_x = px + 10
  local cb_y = py + 26
  if mx >= cb_x and mx <= cb_x + 80 and
     my >= cb_y and my <= cb_y + 12 then
    make_noise = not make_noise
    local config = fetch("/appdata/poingo/config.pod") or {}
    config.make_noise = make_noise
    store("/appdata/poingo/config.pod", config)
  end
  
  local cb2_x = px + 110
  local cb2_y = py + 26
  if mx >= cb2_x and mx <= cb2_x + 80 and
     my >= cb2_y and my <= cb2_y + 12 then
    fullscreen = not fullscreen
    fullscreen_changed = (fullscreen ~= fullscreen_initial)
    local config = fetch("/appdata/poingo/config.pod") or {}
    config.fullscreen = fullscreen
    store("/appdata/poingo/config.pod", config)
  end
end

function close_settings_panel()
  show_settings_panel = false
  
  local config = fetch("/appdata/poingo/config.pod") or {}
  config.make_noise = make_noise
  config.fullscreen = fullscreen
  store("/appdata/poingo/config.pod", config)
end

function sim(DW, DH, all_prop)
  if DH ~= lastDH then
    local sy = DH / lastDH
    ball_y = ball_y * sy
    vy = vy * sy
    GRAV = GRAV_FRAC * DH
    lastDH = DH
  end

  vy = vy + GRAV
  ball_x, ball_y = ball_x + vx, ball_y + vy

  if ball_x < 0 then
    ball_x = -ball_x
    vx = -vx
    if make_noise then
      note(13, 1, 16, 0, 0, 3)
      note(11, 1, 16, 0, 0, 4)
    end
  elseif ball_x + all_prop > DW then
    ball_x = (DW - all_prop) - ((ball_x + all_prop) - DW)
    vx = -vx
    if make_noise then
      note(13, 1, 16, 0, 0, 3)
      note(11, 1, 16, 0, 0, 4)
    end
  end

  if (ball_y + all_prop > ((DH / 20) * 18)) and (vy > 0) then
    ball_y = (((DH / 20) * 18) - all_prop) - ((ball_y + all_prop) - ((DH / 20) * 18))
    vy = -vy
    if make_noise then
      note(13, 1, 32, 0, 0, 1)
      note(11, 1, 32, 0, 0, 2)
    end
  end
end

function _draw()
  local disp = get_display()
  local DW, DH = disp:width(), disp:height()

  local GRAY_COL = 5
  rectfill(0, 0, DW - 1, DH - 1, GRAY_COL)

  local GRID_COL = 6
  local slice_x = (DW / 20)
  local slice_y = (DH / 20)
  local two_slice_x = slice_x * 2
  local two_slice_y = slice_y * 2
  local eighteen_slice_x = slice_x * 18
  local eighteen_slice_y = slice_y * 18

  if (DW > 50 and DH > 50) then
    for x = 2, 18 do
      local sx = slice_x * x
      rectfill(sx, two_slice_y, sx, eighteen_slice_y, GRID_COL)
    end
    for y = 2, 18 do
      local sy = slice_y * y
      rectfill(two_slice_x, sy, eighteen_slice_x, sy, GRID_COL)
    end
  else
    local PAT_A = 0xA5A5
    fillp(PAT_A)
    rectfill(two_slice_x, two_slice_y, eighteen_slice_x, eighteen_slice_y, GRID_COL)
    fillp()
    rectfill(two_slice_x, two_slice_y, eighteen_slice_x, two_slice_y, GRID_COL)
    rectfill(two_slice_x, eighteen_slice_y, eighteen_slice_x, eighteen_slice_y, GRID_COL)
    rectfill(two_slice_x, two_slice_y, two_slice_x, eighteen_slice_y, GRID_COL)
    rectfill(eighteen_slice_x, two_slice_y, eighteen_slice_x, eighteen_slice_y, GRID_COL)
  end

  local prop_x = DW / CANV_W
  local prop_y = DH / CANV_H
  local total_prop = math.min(prop_x, prop_y)

  local ballw_prop = prop_x * BALL_W
  local ballh_prop = prop_y * BALL_H
  local all_prop = math.min(ballw_prop, ballh_prop)

  phase_f = (phase_f + ADV_PER_TICK) % FRAME_COUNT
  phase_i = math.floor(phase_f)

  if not ball_frames[phase_i] then
    build_frame_pair_shared(phase_i)
    return
  end

  sim(DW, DH, all_prop)

  local bw = math.floor(all_prop + 0.5)
  local bh = bw
  local bx = math.floor(ball_x + 0.5)
  local by = math.floor(ball_y + 0.5)

  local OFF = math.floor(total_prop * 8 + 0.5)
  local sx = bx + OFF
  local sy = by + OFF

  local SHADOW_COL = 1
  ovalfill(sx, sy, sx + bw, sy + bh, SHADOW_COL)

  local gx1, gy1 = two_slice_x, two_slice_y
  local gx2, gy2 = eighteen_slice_x, eighteen_slice_y

  local scx = sx + bw * 0.5
  local scy = sy + bh * 0.5
  local rx = bw * 0.5
  local ry = bh * 0.5
  local rx2, ry2 = rx*rx, ry*ry

  local function clamp(v, a, b)
    if v < a then return a elseif v > b then return b else return v end
  end

  if (DW > 50 and DH > 50) then
    for xi = 2, 18 do
      local X = slice_x * xi
      local dx = X - scx
      local t = 1.0 - (dx*dx)/rx2
      if t > 0 then
        local dy = ry * math.sqrt(t)
        local yA = clamp(scy - dy, gy1, gy2)
        local yB = clamp(scy + dy, gy1, gy2)
        rectfill(X, math.floor(yA + 0.5), X, math.floor(yB + 0.5), 5)
      end
    end

    for yi = 2, 18 do
      local Y = slice_y * yi
      local dy = Y - scy
      local t = 1.0 - (dy*dy)/ry2
      if t > 0 then
        local dx = rx * math.sqrt(t)
        local xA = clamp(scx - dx, gx1, gx2)
        local xB = clamp(scx + dx, gx1, gx2)
        rectfill(math.floor(xA + 0.5), Y, math.floor(xB + 0.5), Y, 5)
      end
    end
  else
    local PAT_A = 0xA5A5
    fillp(PAT_A)
    ovalfill(sx, sy, sx + bw, sy + bh, 5)
    fillp()
    
    local dy = two_slice_y - scy
    local t = 1.0 - (dy*dy)/ry2
    if t > 0 then
      local dx = rx * math.sqrt(t)
      local xA = clamp(scx - dx, gx1, gx2)
      local xB = clamp(scx + dx, gx1, gx2)
      rectfill(math.floor(xA + 0.5), two_slice_y, math.floor(xB + 0.5), two_slice_y, 5)
    end
    
    dy = eighteen_slice_y - scy
    t = 1.0 - (dy*dy)/ry2
    if t > 0 then
      local dx = rx * math.sqrt(t)
      local xA = clamp(scx - dx, gx1, gx2)
      local xB = clamp(scx + dx, gx1, gx2)
      rectfill(math.floor(xA + 0.5), eighteen_slice_y, math.floor(xB + 0.5), eighteen_slice_y, 5)
    end
    
    local dx = two_slice_x - scx
    t = 1.0 - (dx*dx)/rx2
    if t > 0 then
      dy = ry * math.sqrt(t)
      local yA = clamp(scy - dy, gy1, gy2)
      local yB = clamp(scy + dy, gy1, gy2)
      rectfill(two_slice_x, math.floor(yA + 0.5), two_slice_x, math.floor(yB + 0.5), 5)
    end
    
    dx = eighteen_slice_x - scx
    t = 1.0 - (dx*dx)/rx2
    if t > 0 then
      dy = ry * math.sqrt(t)
      local yA = clamp(scy - dy, gy1, gy2)
      local yB = clamp(scy + dy, gy1, gy2)
      rectfill(eighteen_slice_x, math.floor(yA + 0.5), eighteen_slice_x, math.floor(yB + 0.5), 5)
    end
  end

  local frame = ball_frames[phase_i]
  sspr(frame, 0, 0, BALL_W, BALL_H, bx, by, bw, bh)
  
  if show_settings_panel and DW >= 160 and DH >= 120 then
    draw_settings_panel(DW, DH)
  end
end

function draw_settings_panel(DW, DH)
  local pw = math.min(200, DW - 40)
  local ph = math.min(120, DH - 40)
  local px = (DW - pw) / 2
  local py = (DH - ph) / 2
  
  rectfill(px, py, px + pw, py + ph, 7)
  rect(px, py, px + pw, py + ph, 0)
  
  print("Press any key to show/hide this config", px + 6, py + 6, 0)
  
  local mx, my = mouse()
  
  local cb_x = px + 10
  local cb_y = py + 26
  local cb_hover = mx >= cb_x and mx <= cb_x + 80 and
                   my >= cb_y and my <= cb_y + 12
  
  if cb_hover then
    rectfill(cb_x - 2, cb_y - 2, cb_x + 82, cb_y + 14, 6)
    rect(cb_x - 2, cb_y - 2, cb_x + 82, cb_y + 14, 5)
  end
  
  rect(cb_x, cb_y, cb_x + 12, cb_y + 12, 12)
  
  if make_noise then
    line(cb_x + 2, cb_y + 6, cb_x + 5, cb_y + 9, 12)
    line(cb_x + 5, cb_y + 9, cb_x + 10, cb_y + 3, 12)
  end
  
  print("Make noise", cb_x + 18, cb_y + 2, 12)
  
  local cb2_x = px + 110
  local cb2_y = py + 26
  local cb2_hover = mx >= cb2_x and mx <= cb2_x + 80 and
                    my >= cb2_y and my <= cb2_y + 12
  
  if cb2_hover then
    rectfill(cb2_x - 2, cb2_y - 2, cb2_x + 82, cb2_y + 14, 6)
    rect(cb2_x - 2, cb2_y - 2, cb2_x + 82, cb2_y + 14, 5)
  end
  
  rect(cb2_x, cb2_y, cb2_x + 12, cb2_y + 12, 17)
  
  if fullscreen then
    line(cb2_x + 2, cb2_y + 6, cb2_x + 5, cb2_y + 9, 17)
    line(cb2_x + 5, cb2_y + 9, cb2_x + 10, cb2_y + 3, 17)
  end
  
  print("Fullscreen", cb2_x + 18, cb2_y + 2, 17)
  
  if fullscreen_changed then
    print("Change will take effect on next start", px + 10, py + 46, 17)
  end
  
  local inst_y = py + 60
  print("I can be a wallpaper or screensaver,", px + 10, inst_y, 1)
  inst_y += 10
  print("or even be dragged into the tooltray!", px + 10, inst_y, 1)
  
  local btn_w = 120
  local btn_h = 20
  local btn_x = px + (pw - btn_w) / 2
  local btn_y = py + ph - 26
  local btn_hover = mx >= btn_x and mx <= btn_x + btn_w and
                    my >= btn_y and my <= btn_y + btn_h
  
  if btn_hover then
    rectfill(btn_x - 2, btn_y - 2, btn_x + btn_w + 2, btn_y + btn_h + 2, 6)
    rect(btn_x - 2, btn_y - 2, btn_x + btn_w + 2, btn_y + btn_h + 2, 5)
  end
  
  rectfill(btn_x, btn_y, btn_x + btn_w, btn_y + btn_h, 7)
  rect(btn_x, btn_y, btn_x + btn_w, btn_y + btn_h, 0)
  
  local btn_text = "Close"
  local text_w = #btn_text * 4
  print(btn_text, btn_x + (btn_w - text_w) / 2, btn_y + 6, 0)
end