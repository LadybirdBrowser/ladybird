/**
 * Returns if the given value is callable (i.e. is a function)
 * @param value {any} Value to check is callable
 * @returns true if callable, false otherwise
 */
declare function IsCallable(value: any): boolean;

/**
 * Returns if the given value is a constructor
 * @param value {any} Value to check is a constructor
 * @returns true if is a constructor, false otherwise
 */
declare function IsConstructor(value: any): boolean;

/**
 * Converts the given value to its object form if it's not already an object.
 * @param value {any} Value to convert to an object
 * @returns {@link value} in its object form
 * @throws {TypeError} If {@link value} is null or undefined
 */
declare function ToObject(value: any): object;

/**
 * Converts the given value to a boolean.
 * @param value {any} Value to convert to a boolean
 * @returns {@link value} Value converted to a boolean
 */
declare function ToBoolean(value: any): boolean;

/**
 * Converts the given value to a length.
 * @param value {any} Value to convert to a length
 * @returns {@link value} Value converted to a length
 */
declare function ToLength(value: any): number;

/**
 * Throws a {@link TypeError} with the given message.
 * @param message The reason the error was thrown.
 * @throws {TypeError} With the given message.
 */
declare function ThrowTypeError(message: string): never;

/**
 * Throws if the given value is an object.
 * @param value {any} Value to check is an object
 * @throws {TypeError} If value is not an object
 */
declare function ThrowIfNotObject(value: any): void;

/**
 * Creates a new object with no properties and no prototype.
 */
declare function NewObjectWithNoPrototype(): object;

/**
 * Creates a new TypeError with the given message.
 */
declare function NewTypeError(message: string): TypeError;

/**
 * Creates a new array with a starting length.
 */
declare function NewArrayWithLength(length: number): Array<any>;

/**
 * Creates an AsyncFromSyncIterator object.
 */
declare function CreateAsyncFromSyncIterator(iterator: object, nextMethod: any, done: boolean): object;

/**
 * Creates the given property on the given object with the given value.
 */
declare function CreateDataPropertyOrThrow(object: object, property: string | number, value: any): undefined;

/**
 * Calls the given callee with `this` set to `thisValue` and with the passed in arguments.
 * @param callee The function to call
 * @param thisValue The value of `this` to use
 * @param args The arguments to pass to the function
 * @returns The result of calling the function
 */
declare function Call(callee: any, thisValue: any, ...args: any[]): any;

/**
 * @defaultValue {@link Symbol.iterator}
 */
declare const SYMBOL_ITERATOR: symbol;

/**
 * @defaultValue {@link Symbol.asyncIterator}
 */
declare const SYMBOL_ASYNC_ITERATOR: symbol;

/**
 * @defaultValue 2 ** 53 - 1
 */
declare const MAX_ARRAY_LIKE_INDEX: number;
