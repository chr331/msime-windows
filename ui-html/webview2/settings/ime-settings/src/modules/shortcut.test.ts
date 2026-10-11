import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { applyShortcutConfig, setupShortcut } from './shortcut';
import { updateConfig } from './config-sync';

vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));

class Checkbox extends EventTarget {
  checked = true;
  textContent = '';
  private attributes = new Map<string, string>();
  getAttribute(name: string): string | null { return this.attributes.get(name) ?? null; }
  setAttribute(name: string, value: string): void { this.attributes.set(name, value); }
}

let characterSet: Checkbox;
let trilingualCycle: Checkbox;
let editor: Record<string, Checkbox>;
beforeEach(() => {
  vi.clearAllMocks();
  characterSet = new Checkbox();
  trilingualCycle = new Checkbox();
  trilingualCycle.checked = false;
  editor = Object.fromEntries(['punctuationHotkeyButton', 'punctuationHotkeyClear', 'punctuationHotkeyReset',
    'punctuationHotkeyStatus'].map(id => [id, new Checkbox()]));
  vi.stubGlobal('document', {
    getElementById: (id: string) => {
      if (id === 'characterSetShortcutCheckbox') return characterSet;
      if (id === 'trilingualCycleCheckbox') return trilingualCycle;
      return editor[id] ?? null;
    },
    querySelectorAll: () => [],
  });
  applyShortcutConfig({});
});
afterEach(() => vi.unstubAllGlobals());

it('applies the persisted shortcut setting without writing it back', () => {
  applyShortcutConfig({ toggle_character_set_ctrl_shift_f: false });
  expect(characterSet.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
  applyShortcutConfig({ toggle_character_set_ctrl_shift_f: true });
  expect(characterSet.checked).toBe(true);
});

it('writes only the character-set shortcut setting when the user disables it', () => {
  setupShortcut();
  characterSet.checked = false;
  characterSet.dispatchEvent(new Event('change'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_character_set_ctrl_shift_f', false);
});

it('applies the persisted trilingual cycle switch without writing it back', () => {
  applyShortcutConfig({ trilingual_cycle: true });
  expect(trilingualCycle.checked).toBe(true);
  applyShortcutConfig({ trilingual_cycle: false });
  expect(trilingualCycle.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
});

it('writes only the trilingual cycle switch when the user toggles it', () => {
  setupShortcut();
  trilingualCycle.checked = true;
  trilingualCycle.dispatchEvent(new Event('change'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.trilingual_cycle', true);
});

it('accepts older configuration snapshots with no shortcut field', () => {
  applyShortcutConfig(undefined);
  applyShortcutConfig({ switch_language_shift: false });
  expect(characterSet.checked).toBe(true);
  expect(trilingualCycle.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
});

function key(code: string, options: Record<string, unknown> = {}): Event {
  const event = Object.assign(new Event('keydown', { cancelable: true }), {
    code, ctrlKey: true, altKey: false, shiftKey: false, metaKey: false, repeat: false,
    getModifierState: () => false, ...options,
  });
  editor.punctuationHotkeyButton.dispatchEvent(event);
  return event;
}

it('applies default, custom and explicitly disabled bindings without writing back', () => {
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  applyShortcutConfig({ toggle_punctuation: 'Alt+KeyA' });
  expect(editor.punctuationHotkeyButton.textContent).toBe('Alt + A');
  applyShortcutConfig({ toggle_punctuation: '' });
  expect(editor.punctuationHotkeyButton.textContent).toBe('未绑定');
  expect(updateConfig).not.toHaveBeenCalled();
});

it('records one new chord and waits for the persisted host snapshot', () => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('Period', { shiftKey: true });
  key('Period', { shiftKey: true, repeat: true });
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+Shift+Period');
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  applyShortcutConfig({ toggle_punctuation: 'Ctrl+Shift+Period' });
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + Shift + .');
});

it('records Win32 keys with no scan code and cancels them with Escape', () => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('', { keyCode: 17 });
  key('', { keyCode: 16, shiftKey: true });
  expect(editor.punctuationHotkeyStatus.textContent).toContain('Esc');
  key('', { keyCode: 190, shiftKey: true });
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+Shift+Period');
  applyShortcutConfig({ toggle_punctuation: 'Ctrl+Shift+Period' });
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  expect(key('', { keyCode: 9 }).defaultPrevented).toBe(false);
  key('', { keyCode: 27 });
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + Shift + .');
  expect(editor.punctuationHotkeyButton.getAttribute('aria-pressed')).toBe('false');
  expect(updateConfig).toHaveBeenCalledTimes(1);
});

it('uses the virtual key fallback only for an empty code and keeps conflict checks', () => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('', { keyCode: 70, shiftKey: true });
  key('', { keyCode: 190, altKey: true });
  key('NumpadDecimal', { keyCode: 190 });
  expect(updateConfig).not.toHaveBeenCalled();
  key('', { keyCode: 71 });
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+KeyG');
});

it.each(['punctuationHotkeyClear', 'punctuationHotkeyReset'])('writes the %s action', id => {
  setupShortcut();
  editor[id].dispatchEvent(new Event('click'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation',
    id === 'punctuationHotkeyClear' ? '' : 'Ctrl+Period');
});

it('preserves the confirmed value if saving fails and the host resends it', () => {
  setupShortcut();
  editor.punctuationHotkeyClear.dispatchEvent(new Event('click'));
  applyShortcutConfig({ toggle_punctuation: 'Ctrl+Period' });
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  expect(editor.punctuationHotkeyStatus.textContent).toBe('');
});

it('cancels on Escape or blur, leaves Tab navigation alone and ignores keys outside recording', () => {
  setupShortcut();
  expect(key('Period').defaultPrevented).toBe(false);
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('Escape');
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  expect(key('Tab').defaultPrevented).toBe(false);
  editor.punctuationHotkeyButton.dispatchEvent(new Event('blur'));
  key('Period');
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
});

it('rejects reserved, bare, Win and AltGr chords and ignores modifiers and repeats', () => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('ControlLeft');
  key('Period', { repeat: true });
  key('KeyF', { shiftKey: true });
  key('KeyE', { shiftKey: true });
  key('F9');
  key('F4', { ctrlKey: false, altKey: true });
  key('Period', { ctrlKey: false });
  key('Period', { metaKey: true });
  key('Period', { altKey: true });
  key('Period', { getModifierState: () => true });
  expect(updateConfig).not.toHaveBeenCalled();
  key('Digit2', { ctrlKey: false, altKey: true });
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Alt+Digit2');
});
