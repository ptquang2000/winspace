local build_win = nil

vim.keymap.set("n", "<leader>m", function()
	local cmd = vim.fn.has("win32") == 1 and { "powershell.exe", "-command", "./build.ps1" } or { "sh", "./build.sh" }

	local old_buf = vim.fn.bufnr("winspace://build")
	if old_buf ~= -1 then
		vim.api.nvim_buf_delete(old_buf, { force = true })
	end

	local new_buf = vim.api.nvim_create_buf(false, true)
	vim.api.nvim_buf_call(new_buf, function()
		vim.fn.jobstart(cmd, { term = true })
	end)
	vim.api.nvim_buf_set_name(new_buf, "winspace://build")

	if not (build_win and vim.api.nvim_win_is_valid(build_win)) then
		build_win = vim.api.nvim_open_win(new_buf, false, { split = "right", win = -1 })
	else
		vim.api.nvim_win_set_buf(build_win, new_buf)
	end
end, { desc = "Build winspace" })
