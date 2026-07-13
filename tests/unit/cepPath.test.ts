import { describe, expect, it } from 'vitest';
import { systemPathToNative } from '../../src/host/cepPath';

describe('systemPathToNative', () => {
  it('converts a Windows file:// URI to a plain drive path', () => {
    expect(systemPathToNative('file:///C:/Users/kylen/AppData/Roaming')).toBe(
      'C:/Users/kylen/AppData/Roaming',
    );
  });

  it('decodes percent-escapes such as a space in the username', () => {
    expect(systemPathToNative('file:///C:/Users/Ada%20Lovelace/AppData/Roaming')).toBe(
      'C:/Users/Ada Lovelace/AppData/Roaming',
    );
  });

  it('preserves the leading slash of a POSIX file:// URI', () => {
    expect(systemPathToNative('file:///home/kylen/.config')).toBe('/home/kylen/.config');
  });

  it('passes an already-plain Windows path through unchanged', () => {
    expect(systemPathToNative('C:/Users/kylen/AppData/Roaming')).toBe(
      'C:/Users/kylen/AppData/Roaming',
    );
  });

  it('passes an already-plain POSIX path through unchanged (no spurious decode)', () => {
    // A literal %20 in a plain path must not be decoded - it was never encoded.
    expect(systemPathToNative('/var/tmp/a%20b')).toBe('/var/tmp/a%20b');
  });

  it('handles a lowercase drive letter', () => {
    expect(systemPathToNative('file:///d:/Renders/temp')).toBe('d:/Renders/temp');
  });
});
