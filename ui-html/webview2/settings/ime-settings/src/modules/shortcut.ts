import { updateConfig } from './config-sync';

const DEFAULT_PUNCTUATION_HOTKEY = 'Ctrl+Period';
const PUNCTUATION_KEYS: Record<string, string> = {
  Semicolon: ';', Equal: '=', Comma: ',', Minus: '-', Period: '.', Slash: '/',
  Backquote: '`', BracketLeft: '[', Backslash: '\\', BracketRight: ']', Quote: "'",
};
let punctuationHotkey = DEFAULT_PUNCTUATION_HOTKEY;

function displayPunctuationHotkey(): void {
  const button = document.getElementById('punctuationHotkeyButton');
  if (button && button.getAttribute('aria-pressed') !== 'true') {
    button.textContent = punctuationHotkey ? punctuationHotkey.split('+').map(key =>
      PUNCTUATION_KEYS[key] ?? key.replace(/^(Key|Digit)/, '')).join(' + ') : '未绑定';
  }
}

function punctuationEventCode(event: KeyboardEvent): string {
  if (event.code) return event.code;
  // Win32 keystrokes without a scan code have an empty code in WebView2.
  // Its legacy keyCode still carries the virtual key used by the TSF matcher.
  const vk = event.keyCode;
  if (vk >= 65 && vk <= 90) return `Key${String.fromCharCode(vk)}`;
  if (vk >= 48 && vk <= 57) return `Digit${vk - 48}`;
  if (vk >= 112 && vk <= 123) return `F${vk - 111}`;
  return ({
    9: 'Tab', 16: 'ShiftLeft', 17: 'ControlLeft', 18: 'AltLeft', 27: 'Escape',
    91: 'MetaLeft', 92: 'MetaRight', 186: 'Semicolon', 187: 'Equal', 188: 'Comma',
    189: 'Minus', 190: 'Period', 191: 'Slash', 192: 'Backquote', 219: 'BracketLeft',
    220: 'Backslash', 221: 'BracketRight', 222: 'Quote',
  } as Record<number, string>)[vk] ?? '';
}

function recordedPunctuationHotkey(event: KeyboardEvent, code: string): string | null {
  if (event.metaKey || event.ctrlKey === event.altKey || event.getModifierState('AltGraph')) return null;
  if (!/^(Key[A-Z]|Digit[0-9]|F([1-9]|1[0-2]))$/.test(code) && !(code in PUNCTUATION_KEYS)) return null;
  const binding = `${event.ctrlKey ? 'Ctrl' : 'Alt'}+${event.shiftKey ? 'Shift+' : ''}${code}`;
  return ['Ctrl+Shift+KeyE', 'Ctrl+Shift+KeyF', 'Ctrl+F9', 'Alt+F4'].includes(binding) ? null : binding;
}

function setupPunctuationHotkey(): void {
  const button = document.getElementById('punctuationHotkeyButton') as HTMLButtonElement | null;
  const status = document.getElementById('punctuationHotkeyStatus');
  if (!button) return;
  const finish = (message = '') => {
    button.setAttribute('aria-pressed', 'false');
    displayPunctuationHotkey();
    if (status) status.textContent = message;
  };
  const save = (binding: string) => {
    finish('正在保存…');
    updateConfig('keybindings.toggle_punctuation', binding);
    // The host returns the persisted snapshot on success AND failure.
    // Keep showing the previous value until that snapshot confirms the write.
  };
  button.addEventListener('click', () => {
    if (button.getAttribute('aria-pressed') === 'true') return finish();
    button.setAttribute('aria-pressed', 'true');
    button.textContent = '请按下快捷键';
    if (status) status.textContent = 'Ctrl 或 Alt + 主键，可加 Shift；Esc 取消，Tab 离开。';
  });
  button.addEventListener('blur', () => finish());
  button.addEventListener('keydown', event => {
    const code = punctuationEventCode(event);
    if (button.getAttribute('aria-pressed') !== 'true' || code === 'Tab') return;
    event.preventDefault();
    event.stopPropagation();
    if (code === 'Escape') return finish();
    if (event.repeat || /^(Control|Shift|Alt|Meta)(Left|Right)$/.test(code)) return;
    const binding = recordedPunctuationHotkey(event, code);
    if (binding === null) {
      if (status) status.textContent = '此组合不支持或已被系统/输入法占用，请选择其他组合。';
      return;
    }
    save(binding);
  });
  document.getElementById('punctuationHotkeyClear')?.addEventListener('click', () => save(''));
  document.getElementById('punctuationHotkeyReset')?.addEventListener('click', () => save(DEFAULT_PUNCTUATION_HOTKEY));
}

export function applyShortcutConfig(config: {
  switch_language_shift?: boolean;
  switch_language_ctrl?: boolean;
  switch_language_ctrl_alt_space?: boolean;
  toggle_character_set_ctrl_shift_f?: boolean;
  trilingual_cycle?: boolean;
  toggle_punctuation?: string;
} | undefined): void {
  if (!config) {
    return;
  }
  const shift = document.getElementById('switchLanguageShiftCheckbox') as HTMLInputElement | null;
  const ctrl = document.getElementById('switchLanguageCtrlCheckbox') as HTMLInputElement | null;
  const ctrlAltSpace = document.getElementById('switchLanguageCtrlAltSpaceCheckbox') as HTMLInputElement | null;
  const characterSet = document.getElementById('characterSetShortcutCheckbox') as HTMLInputElement | null;
  const trilingualCycle = document.getElementById('trilingualCycleCheckbox') as HTMLInputElement | null;
  if (characterSet && typeof config.toggle_character_set_ctrl_shift_f === 'boolean') {
    characterSet.checked = config.toggle_character_set_ctrl_shift_f;
  }
  if (shift && typeof config.switch_language_shift === 'boolean') {
    shift.checked = config.switch_language_shift;
  }
  if (ctrl && typeof config.switch_language_ctrl === 'boolean') {
    ctrl.checked = config.switch_language_ctrl;
  }
  if (ctrlAltSpace && typeof config.switch_language_ctrl_alt_space === 'boolean') {
    ctrlAltSpace.checked = config.switch_language_ctrl_alt_space;
  }
  if (trilingualCycle && typeof config.trilingual_cycle === 'boolean') {
    trilingualCycle.checked = config.trilingual_cycle;
  }
  punctuationHotkey = typeof config.toggle_punctuation === 'string' ? config.toggle_punctuation : DEFAULT_PUNCTUATION_HOTKEY;
  displayPunctuationHotkey();
  const status = document.getElementById('punctuationHotkeyStatus');
  if (status?.textContent === '正在保存…') status.textContent = '';
}

export function setupShortcut(): void {
  setupPunctuationHotkey();
  const characterSet = document.getElementById('characterSetShortcutCheckbox') as HTMLInputElement | null;
  characterSet?.addEventListener('change', () => {
    updateConfig('keybindings.toggle_character_set_ctrl_shift_f', characterSet.checked);
  });
  const trilingualCycle = document.getElementById('trilingualCycleCheckbox') as HTMLInputElement | null;
  trilingualCycle?.addEventListener('change', () => {
    updateConfig('keybindings.trilingual_cycle', trilingualCycle.checked);
  });
  const mapping: Record<string, string> = {
    shift: 'keybindings.switch_language_shift',
    ctrl: 'keybindings.switch_language_ctrl',
    'ctrl-alt-space': 'keybindings.switch_language_ctrl_alt_space'
  };
  document.querySelectorAll<HTMLInputElement>('input[name="switch-language"]').forEach((checkbox) => {
    checkbox.addEventListener('change', () => {
      const path = mapping[checkbox.value];
      if (path) {
        updateConfig(path, checkbox.checked);
      }
    });
  });
}
